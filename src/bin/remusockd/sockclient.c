#include "config.h"
#include "log.h"
#include "sockclient.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

struct SockClient
{
    int fd;
};

SockClient *SockClient_create(const Config *config)
{
    int fd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
	logmsg(L_ERROR, "sockclient: cannot create socket");
	return 0;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, config->sockname, sizeof addr.sun_path - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0)
    {
	logfmt(L_ERROR, "sockclient: error connecting to `%s'", addr.sun_path);
	close(fd);
	return 0;
    }

    SockClient *self = xmalloc(sizeof *self);
    self->fd = fd;
    return self;
}

void SockClient_destroy(SockClient *self)
{
    if (!self) return;
    close(self->fd);
    free(self);
}

