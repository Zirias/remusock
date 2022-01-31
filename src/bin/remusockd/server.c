#define _DEFAULT_SOURCE

#include "config.h"
#include "connection.h"
#include "event.h"
#include "log.h"
#include "service.h"
#include "server.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef MAXSOCKS
#define MAXSOCKS (2*MAXBINDS)
#endif

#define CONNCHUNK 8

static char hostbuf[NI_MAXHOST];
static char servbuf[NI_MAXSERV];

struct sockaddr_in sain;
struct sockaddr_in6 sain6;

enum saddrt
{
    ST_UNIX,
    ST_INET,
    ST_INET6
};

struct Server
{
    Event *clientConnected;
    Event *clientDisconnected;
    Connection **conn;
    char *path;
    size_t conncapa;
    size_t connsize;
    int fd[MAXSOCKS];
    enum saddrt st[MAXSOCKS];
    ConnectionCreateMode ccm;
    int numericHosts;
    uint8_t nsocks;
    uint8_t readOffset;
};

static void removeConnection(void *receiver, void *sender, void *args)
{
    (void)args;

    Server *self = receiver;
    Connection *conn = sender;
    for (size_t pos = 0; pos < self->connsize; ++pos)
    {
	if (self->conn[pos] == conn)
	{
	    logfmt(L_DEBUG, "server: client disconnected from %s",
		    Connection_remoteAddr(conn));
	    memmove(self->conn+pos, self->conn+pos+1,
		    (self->connsize - pos) * sizeof *self->conn);
	    --self->connsize;
	    Event_raise(self->clientDisconnected, 0, conn);
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
    int *sockfd = args;
    enum saddrt st = ST_UNIX;
    for (uint8_t n = 0; n < self->nsocks; ++n)
    {
	if (self->fd[n] == *sockfd)
	{
	    st = self->st[n];
	    break;
	}
    }
    socklen_t salen;
    struct sockaddr *sa = 0;
    socklen_t *sl = 0;
    if (st == ST_INET)
    {
	sa = (struct sockaddr *)&sain;
	salen = sizeof sain;
	sl = &salen;
    }
    else if (st == ST_INET6)
    {
	sa = (struct sockaddr *)&sain6;
	salen = sizeof sain6;
	sl = &salen;
    }
    int connfd = accept(*sockfd, sa, sl);
    if (connfd < 0)
    {
	logmsg(L_WARNING, "server: failed to accept connection");
	return;
    }
    fcntl(connfd, F_SETFL, fcntl(connfd, F_GETFL, 0) | O_NONBLOCK);
    if (self->connsize == self->conncapa)
    {
	self->conncapa += CONNCHUNK;
	self->conn = xrealloc(self->conn, self->conncapa * sizeof *self->conn);
    }
    Connection *newconn = Connection_create(connfd, self->ccm,
	    self->readOffset);
    self->conn[self->connsize++] = newconn;
    Event_register(Connection_closed(newconn), self, removeConnection, 0);
    if (self->path)
    {
	Connection_setRemoteAddrStr(newconn, self->path);
    }
    else if (sa)
    {
	Connection_setRemoteAddr(newconn, sa, salen, self->numericHosts);
    }
    logfmt(L_DEBUG, "server: client connected from %s",
	    Connection_remoteAddr(newconn));
    Event_raise(self->clientConnected, 0, newconn);
}

Server *Server_create(uint8_t nsocks, int *sockfd, enum saddrt *st,
	ConnectionCreateMode mode, char *path, int numericHosts,
	uint8_t readOffset)
{
    if (nsocks < 1 || nsocks > MAXSOCKS)
    {
	free(path);
	return 0;
    }
    Server *self = xmalloc(sizeof *self);
    self->clientConnected = Event_create(self);
    self->clientDisconnected = Event_create(self);
    self->conn = xmalloc(CONNCHUNK * sizeof *self->conn);
    self->path = path;
    self->conncapa = CONNCHUNK;
    self->connsize = 0;
    self->ccm = mode;
    self->numericHosts = numericHosts;
    self->nsocks = nsocks;
    self->readOffset = readOffset;
    memcpy(self->fd, sockfd, nsocks * sizeof *sockfd);
    memcpy(self->st, st, nsocks * sizeof *st);
    for (uint8_t i = 0; i < nsocks; ++i)
    {
	Event_register(Service_readyRead(), self, acceptConnection, sockfd[i]);
	Service_registerRead(sockfd[i]);
    }

    return self;
}

Server *Server_createTcp(const Config *config, ConnectionCreateMode mode,
	uint8_t readOffset)
{
    int fd[MAXSOCKS];
    enum saddrt st[MAXSOCKS];

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE|AI_ADDRCONFIG|AI_NUMERICSERV;
    char portstr[6];
    snprintf(portstr, 6, "%d", config->port);

    struct addrinfo *res0;
    int nsocks = 0;
    int bi = 0;
    int opt_true = 1;
    do
    {
	res0 = 0;
	if (getaddrinfo(config->bindaddr[bi], portstr, &hints, &res0) < 0
		|| !res0)
	{
	    logfmt(L_ERROR, "server: cannot get address info for `%s'",
		    config->bindaddr[bi]);
	    continue;
	}
	for (struct addrinfo *res = res0; res && nsocks < MAXSOCKS;
		res = res->ai_next)
	{
	    if (res->ai_family != AF_INET && res->ai_family != AF_INET6)
	    {
		continue;
	    }
	    fd[nsocks] = socket(res->ai_family, res->ai_socktype,
		    res->ai_protocol);
	    if (fd[nsocks] < 0)
	    {
		logmsg(L_ERROR, "server: cannot create socket");
		continue;
	    }
	    fcntl(fd[nsocks], F_SETFL,
		    fcntl(fd[nsocks], F_GETFL, 0) | O_NONBLOCK);
	    if (setsockopt(fd[nsocks], SOL_SOCKET, SO_REUSEADDR,
			&opt_true, sizeof opt_true) < 0)
	    {
		logmsg(L_ERROR, "server: cannot set socket option");
		close(fd[nsocks]);
		continue;
	    }
#ifdef IPV6_V6ONLY
	    if (res->ai_family == AF_INET6)
	    {
		setsockopt(fd[nsocks], IPPROTO_IPV6, IPV6_V6ONLY,
			&opt_true, sizeof opt_true);
	    }
#endif
	    if (bind(fd[nsocks], res->ai_addr, res->ai_addrlen) < 0)
	    {
		logmsg(L_ERROR, "server: cannot bind to specified address");
		close(fd[nsocks]);
		continue;
	    }
	    if (listen(fd[nsocks], 8) < 0)
	    {
		logmsg(L_ERROR, "server: cannot listen on socket");
		close(fd[nsocks]);
		continue;
	    }
	    const char *addrstr = "<unknown>";
	    if (getnameinfo(res->ai_addr, res->ai_addrlen,
			hostbuf, sizeof hostbuf,
			servbuf, sizeof servbuf,
			NI_NUMERICHOST|NI_NUMERICSERV) >= 0)
	    {
		addrstr = hostbuf;
	    }
	    logfmt(L_INFO, "server: listening on %s port %s",
		    addrstr, portstr);
	    st[nsocks++] = res->ai_family == AF_INET ? ST_INET : ST_INET6;
	}
	freeaddrinfo(res0);
    } while (++bi < MAXBINDS && config->bindaddr[bi]);
    if (!nsocks)
    {
	logmsg(L_ERROR, "server: could not create any socket");
	return 0;
    }
    
    Server *self = Server_create(nsocks, fd, st, mode, 0,
	    config->numericHosts, readOffset);
    return self;
}

Server *Server_createUnix(const Config *config, ConnectionCreateMode mode,
	uint8_t readOffset)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        logmsg(L_ERROR, "server: cannot create socket");
        return 0;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

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

	fd_set wfds;
	FD_ZERO(&wfds);
	FD_SET(fd, &wfds);
	struct timeval tv;
	memset(&tv, 0, sizeof tv);
	tv.tv_usec = 300000U;
	int sockerr = 0;
	socklen_t sockerrlen = sizeof sockerr;
	errno = 0;
	if (connect(fd, (struct sockaddr *)&addr, sizeof addr) >= 0
		|| (errno == EINPROGRESS
		    && select(fd + 1, 0, &wfds, 0, &tv) > 0
		    && getsockopt(fd, SOL_SOCKET, SO_ERROR,
			&sockerr, &sockerrlen) >= 0
		    && !sockerr))
        {
            logfmt(L_ERROR, "server: `%s' is already opened for listening",
                    addr.sun_path);
            close(fd);
            return 0;
        }
	close(fd);

        if (unlink(addr.sun_path) < 0)
        {
            logfmt(L_ERROR, "server: cannot remove stale socket `%s'",
                    addr.sun_path);
            return 0;
        }

        logfmt(L_WARNING, "server: removed stale socket `%s'",
                addr.sun_path);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
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
    logfmt(L_INFO, "server: listening on %s", addr.sun_path);

    if (chmod(addr.sun_path, config->sockmode) < 0)
    {
	logfmt(L_ERROR, "server: cannot set desired socket permissions");
    }
    if (config->sockuid != -1 || config->sockgid != -1)
    {
	if (chown(addr.sun_path, config->sockuid, config->sockgid) < 0)
	{
	    logfmt(L_ERROR, "server: cannot set desired socket ownership");
	}
    }

    enum saddrt sat = ST_UNIX;
    Server *self = Server_create(1, &fd, &sat, mode,
	    copystr(addr.sun_path), config->numericHosts, readOffset);
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
    for (uint8_t i = 0; i < self->nsocks; ++i)
    {
	Service_unregisterRead(self->fd[i]);
	Event_unregister(Service_readyRead(), self, acceptConnection,
		self->fd[i]);
	close(self->fd[i]);
    }
    Event_destroy(self->clientDisconnected);
    Event_destroy(self->clientConnected);
    if (self->path)
    {
	unlink(self->path);
	free(self->path);
    }
    free(self);
}

