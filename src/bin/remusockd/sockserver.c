#include "config.h"
#include "event.h"
#include "log.h"
#include "service.h"
#include "sockclient.h"
#include "sockserver.h"
#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define CONNCHUNK 8
#define CONNBUFSZ 16384

struct SockServer
{
    int *connfd;
    size_t conncapa;
    size_t connsize;
    int sockfd;
    char path[];
};

static void readConnection(void *receiver, int id,
        const void *sender, const void *args)
{
    (void)sender;
    (void)args;

    SockServer *self = receiver;
    unsigned char buf[CONNBUFSZ];

    int rc = read(id, buf, CONNBUFSZ);
    if (rc > 0)
    {
        write(id, buf, rc);
        return;
    }

    if (rc < 0)
    {
        logmsg(L_WARNING, "sockserver: error reading from connection");
    }
    else
    {
        logmsg(L_INFO, "sockserver: client disconnected");
    }

    Service_unregisterRead(id);
    Event_unregister(Service_readyRead(), self, readConnection, id);
    close(id);
    for (size_t pos = 0; pos < self->connsize; ++pos)
    {
        if (self->connfd[pos] == id)
        {
            --self->connsize;
            memmove(self->connfd+pos, self->connfd+pos+1, self->connsize-pos);
            break;
        }
    }
}

static void acceptConnection(void *receiver, int id,
        const void *sender, const void *args)
{   
    (void)id;
    (void)sender;
    (void)args;
    
    SockServer *self = receiver;
    int connfd = accept(self->sockfd, 0, 0);
    if (connfd < 0)
    {   
        logmsg(L_WARNING, "sockserver: failed to accept connection");
        return;
    }
    if (self->connsize == self->conncapa)
    {   
        self->conncapa += CONNCHUNK;
        self->connfd = xrealloc(self->connfd, self->conncapa);
    }
    self->connfd[self->connsize++] = connfd;
    Event_register(Service_readyRead(), self, readConnection, connfd);
    Service_registerRead(connfd);
    logmsg(L_INFO, "sockserver: client connected");
}

SockServer *SockServer_create(const Config *config)
{
    int fd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
	logmsg(L_ERROR, "sockserver: cannot create socket");
	return 0;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, config->sockname, sizeof addr.sun_path - 1);

    struct stat st;
    errno = 0;
    if (stat(addr.sun_path, &st) >= 0)
    {
	if (!S_ISSOCK(st.st_mode))
	{
	    logfmt(L_ERROR, "sockserver: `%s' exists and is not a socket",
		    addr.sun_path);
	    close(fd);
	    return 0;
	}

	SockClient *client = SockClient_create(config);
	if (client)
	{
	    SockClient_destroy(client);
	    logfmt(L_ERROR, "sockserver: `%s' is already opened for listening",
		    addr.sun_path);
	    close(fd);
	    return 0;
	}

	if (unlink(addr.sun_path) < 0)
	{
	    logfmt(L_ERROR, "sockserver: cannot remove stale socket `%s'",
		    addr.sun_path);
	    close(fd);
	    return 0;
	}

	logfmt(L_WARNING, "sockserver: removed stale socket `%s'",
		addr.sun_path);
    }
    else if (errno != ENOENT)
    {
	logfmt(L_ERROR, "sockserver: cannot access `%s'", addr.sun_path);
	close(fd);
	return 0;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0)
    {
	logfmt(L_ERROR, "sockserver: cannot bind to `%s'", addr.sun_path);
	close(fd);
	return 0;
    }

    if (listen(fd, 8) < 0)
    {
	logfmt(L_ERROR, "sockserver: cannot listen on `%s'", addr.sun_path);
	close(fd);
	return 0;
    }

    SockServer *self = xmalloc(sizeof *self + sizeof addr.sun_path);
    self->connfd = xmalloc(CONNCHUNK * sizeof *self->connfd);
    self->conncapa = CONNCHUNK;
    self->connsize = 0;
    self->sockfd = fd;
    strcpy(self->path, addr.sun_path);
    Event_register(Service_readyRead(), self, acceptConnection, fd);
    Service_registerRead(fd);

    return self;
}

void SockServer_destroy(SockServer *self)
{
    if (!self) return;

    for (size_t pos = 0; pos < self->connsize; ++pos)
    {
	Service_unregisterRead(self->connfd[pos]);
	Event_unregister(Service_readyRead(), self, readConnection,
		self->connfd[pos]);
	close(self->connfd[pos]);
    }
    Service_unregisterRead(self->sockfd);
    Event_unregister(Service_readyRead(), self, acceptConnection,
	    self->sockfd);
    close(self->sockfd);
    unlink(self->path);
    free(self->connfd);
    free(self);
}
