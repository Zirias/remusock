#define _POSIX_C_SOURCE 200112L

#include "client.h"
#include "config.h"
#include "connection.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

Connection *Connection_createTcpClient(const Config *config)
{
    if (!config->remotehost)
    {
	logmsg(L_ERROR, "client: attempt to connect without remote host");
	return 0;
    }
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG|AI_NUMERICSERV;
    char portstr[6];
    snprintf(portstr, 6, "%d", config->port);
    struct addrinfo *res, *res0;
    if (getaddrinfo(config->remotehost, portstr, &hints, &res0) < 0)
    {
	logmsg(L_ERROR, "client: cannot get address info");
	return 0;
    }
    int fd = -1;
    for (res = res0; res; res = res->ai_next)
    {
	if (res->ai_family != AF_INET && res->ai_family != AF_INET6) continue;
	fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (fd < 0) continue;
	int flags = fcntl(fd, F_GETFL, 0);
	int nbflags = flags | O_NONBLOCK;
	fcntl(fd, F_SETFL, nbflags);
	errno = 0;
	if (connect(fd, res->ai_addr, res->ai_addrlen) < 0
		&& errno != EINPROGRESS)
	{
	    close(fd);
	    fd = -1;
	}
	else
	{
	    fcntl(fd, F_SETFL, flags);
	    break;
	}
    }
    freeaddrinfo(res0);
    if (fd < 0)
    {
	logfmt(L_ERROR, "client: cannot connect to `%s'", config->remotehost);
	return 0;
    }
    return Connection_create(fd, 1);
}

Connection *Connection_createUnixClient(const Config *config)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
	logmsg(L_ERROR, "client: cannot create socket");
	return 0;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, config->sockname, sizeof addr.sun_path - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0)
    {
	logfmt(L_ERROR, "client: error connecting to `%s'", addr.sun_path);
	close(fd);
	return 0;
    }

    return Connection_create(fd, 0);
}

