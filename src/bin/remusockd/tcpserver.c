#include "event.h"
#include "log.h"
#include "service.h"
#include "tcpserver.h"
#include "util.h"

#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#define CONNCHUNK 8
#define CONNBUFSZ 16384

struct TcpServer
{
    int *connfd;
    size_t conncapa;
    size_t connsize;
    int sockfd;
};

static void readConnection(void *receiver, int id,
	const void *sender, const void *args)
{
    (void)sender;
    (void)args;

    TcpServer *self = receiver;
    unsigned char buf[CONNBUFSZ];

    int rc = read(id, buf, CONNBUFSZ);
    if (rc > 0)
    {
	write(id, buf, rc);
	return;
    }

    if (rc < 0)
    {
	logmsg(L_WARNING, "tcpserver: error reading from connection");
    }
    else
    {
	logmsg(L_INFO, "tcpserver: client disconnected");
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

    TcpServer *self = receiver;
    int connfd = accept(self->sockfd, 0, 0);
    if (connfd < 0)
    {
	logmsg(L_WARNING, "tcpserver: failed to accept connection");
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
    logmsg(L_INFO, "tcpserver: client connected");
}

TcpServer *TcpServer_create(int port)
{
    int fd = socket(PF_INET6, SOCK_STREAM, 0);
    if (fd < 0)
    {
	logmsg(L_ERROR, "tcpserver: cannot create socket");
	return 0;
    }

    int opt_true = 1;
    int opt_false = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
		&opt_true, sizeof opt_true) < 0
	    || setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
		&opt_false, sizeof opt_false) < 0)
    {
	logmsg(L_ERROR, "tcpserver: cannot set socket options");
	close(fd);
	return 0;
    }

    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof addr);
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0)
    {
	logmsg(L_ERROR, "tcpserver: cannot bind to specified address");
	close(fd);
	return 0;
    }

    if (listen(fd, 8) < 0)
    {
	logmsg(L_ERROR, "tcpserver: cannot listen on socket");
	close(fd);
	return 0;
    }
    
    TcpServer *self = xmalloc(sizeof *self);
    self->connfd = xmalloc(CONNCHUNK * sizeof *self->connfd);
    self->conncapa = CONNCHUNK;
    self->connsize = 0;
    self->sockfd = fd;
    Event_register(Service_readyRead(), self, acceptConnection, fd);
    Service_registerRead(fd);

    return self;
}

void TcpServer_destroy(TcpServer *self)
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
    free(self->connfd);
    free(self);
}
