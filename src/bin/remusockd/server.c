#define _POSIX_C_SOURCE 200112L

#include "client.h"
#include "config.h"
#include "connection.h"
#include "event.h"
#include "log.h"
#include "service.h"
#include "server.h"
#include "util.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define CONNCHUNK 8

struct Server
{
    Event *clientConnected;
    Event *clientDisconnected;
    Connection **conn;
    char *path;
    size_t conncapa;
    size_t connsize;
    int fd;
};

static void removeConnection(void *receiver, void *sender, void *args)
{
    (void)args;

    Server *self = receiver;
    const Connection *conn = sender;
    for (size_t pos = 0; pos < self->connsize; ++pos)
    {
	if (self->conn[pos] == conn)
	{
	    Event_unregister(Connection_closed(self->conn[pos]), self,
		    removeConnection, 0);
	    Event_raise(self->clientDisconnected, 0, self->conn[pos]);
	    Connection_destroy(self->conn[pos]);
	    logmsg(L_INFO, "server: client disconnected");
	    memmove(self->conn+pos, self->conn+pos+1,
		    (self->connsize - pos) * sizeof *self->conn);
	    --self->connsize;
	    return;
	}
    }
    logmsg(L_ERROR, "server: trying to remove non-existing connection");
}

static void acceptConnection(void *receiver, void *sender, void *args)
{
    (void)sender;
    (void)args;

    Server *self = receiver;
    int connfd = accept(self->fd, 0, 0);
    if (connfd < 0)
    {
	logmsg(L_WARNING, "server: failed to accept connection");
	return;
    }
    if (self->connsize == self->conncapa)
    {
	self->conncapa += CONNCHUNK;
	self->conn = xrealloc(self->conn, self->conncapa * sizeof *self->conn);
    }
    Connection *newconn = Connection_create(connfd);
    self->conn[self->connsize++] = newconn;
    Event_register(Connection_closed(newconn), self, removeConnection, 0);
    logmsg(L_INFO, "server: client connected");
    Event_raise(self->clientConnected, 0, newconn);
}

Server *Server_create(int sockfd, char *path)
{
    Server *self = xmalloc(sizeof *self);
    self->clientConnected = Event_create(self);
    self->clientDisconnected = Event_create(self);
    self->conn = xmalloc(CONNCHUNK * sizeof *self->conn);
    self->path = path;
    self->conncapa = CONNCHUNK;
    self->connsize = 0;
    self->fd = sockfd;
    Event_register(Service_readyRead(), self, acceptConnection, sockfd);
    Service_registerRead(sockfd);

    return self;
}

Server *Server_createTcp(const Config *config)
{
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0)
    {
        logmsg(L_ERROR, "server: cannot create socket");
        return 0;
    }

    int opt_true = 1;
    int opt_false = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                &opt_true, sizeof opt_true) < 0
            || setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
                &opt_false, sizeof opt_false) < 0)
    {
        logmsg(L_ERROR, "server: cannot set socket options");
        close(fd);
        return 0;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET6;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE|AI_ADDRCONFIG|AI_V4MAPPED|AI_NUMERICSERV;
    struct addrinfo *res = 0;
    char portstr[6];
    snprintf(portstr, 6, "%d", config->port);
    if (getaddrinfo(config->bindaddr, portstr, &hints, &res) < 0
	    || !res)
    {
        logmsg(L_ERROR, "server: cannot get address info");
        close(fd);
        return 0;
    }

    if (bind(fd, res->ai_addr, res->ai_addrlen) < 0)
    {
        logmsg(L_ERROR, "server: cannot bind to specified address");
        freeaddrinfo(res);
        close(fd);
        return 0;
    }
    freeaddrinfo(res);
    
    if (listen(fd, 8) < 0)
    {   
        logmsg(L_ERROR, "server: cannot listen on socket");
        close(fd);
        return 0;
    }   

    Server *self = Server_create(fd, 0);
    return self;
}

Server *Server_createUnix(const Config *config)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        logmsg(L_ERROR, "server: cannot create socket");
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
            logfmt(L_ERROR, "server: `%s' exists and is not a socket",
                    addr.sun_path);
            close(fd);
            return 0;
        }

        Connection *client = Connection_createUnixClient(config);
        if (client)
        {
            Connection_destroy(client);
            logfmt(L_ERROR, "server: `%s' is already opened for listening",
                    addr.sun_path);
            close(fd);
            return 0;
        }

        if (unlink(addr.sun_path) < 0)
        {
            logfmt(L_ERROR, "server: cannot remove stale socket `%s'",
                    addr.sun_path);
            close(fd);
            return 0;
        }

        logfmt(L_WARNING, "server: removed stale socket `%s'",
                addr.sun_path);
    }
    else if (errno != ENOENT)
    {
        logfmt(L_ERROR, "server: cannot access `%s'", addr.sun_path);
        close(fd);
        return 0;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0)
    {
        logfmt(L_ERROR, "server: cannot bind to `%s'", addr.sun_path);
        close(fd);
        return 0;
    }

    if (listen(fd, 8) < 0)
    {
        logfmt(L_ERROR, "server: cannot listen on `%s'", addr.sun_path);
        close(fd);
        return 0;
    }

    Server *self = Server_create(fd, copystr(addr.sun_path));
    return self;
}

Event *Server_clientConnected(Server *self)
{
    return self->clientConnected;
}

Event *Server_clientDisconnected(Server *self)
{
    return self->clientDisconnected;
}

void Server_destroy(Server *self)
{
    if (!self) return;

    for (size_t pos = 0; pos < self->connsize; ++pos)
    {
	Event_raise(self->clientDisconnected, 0, self->conn[pos]);
	Connection_destroy(self->conn[pos]);
    }
    free(self->conn);
    Service_unregisterRead(self->fd);
    Event_unregister(Service_readyRead(), self, acceptConnection,
	    self->fd);
    Event_destroy(self->clientDisconnected);
    Event_destroy(self->clientConnected);
    close(self->fd);
    if (self->path)
    {
	unlink(self->path);
	free(self->path);
    }
    free(self);
}

