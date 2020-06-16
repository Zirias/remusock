#include "connection.h"
#include "event.h"
#include "eventargs.h"
#include "log.h"
#include "service.h"
#include "util.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONNBUFSZ 4096

typedef struct Connection
{
    Event *closed;
    Event *dataReceived;
    Event *dataSent;
    int fd;
    int wrbufbusy;
    uint16_t wrbuflen;
    char wrbuf[CONNBUFSZ];
    char rdbuf[CONNBUFSZ];
} Connection;

static void writeConnection(void *receiver, const void *sender,
	const void *args)
{
    (void)sender;
    (void)args;

    Connection *self = receiver;
    if (!self->wrbuflen)
    {
	logmsg(L_ERROR, "server: ready to send with empty buffer");
	Service_unregisterWrite(self->fd);
	return;
    }
    int rc = write(self->fd, self->wrbuf, self->wrbuflen);
    if (rc > 0)
    {
	if (rc < self->wrbuflen)
	{
	    memmove(self->wrbuf+rc, self->wrbuf, self->wrbuflen-rc);
	}
	self->wrbuflen -= rc;
	Event_raise(self->dataSent, 0, 0);
	if (!self->wrbuflen) Service_unregisterWrite(self->fd);
	return;
    }

    logmsg(L_WARNING, "server: error writing to connection");
    Event_raise(self->closed, 0, 0);
}

static void readConnection(void *receiver, const void *sender,
	const void *args)
{
    (void)sender;
    (void)args;

    Connection *self = receiver;

    int rc = read(self->fd, self->rdbuf, CONNBUFSZ);
    if (rc > 0)
    {
	DataReceivedEventArgs rargs = {
	    .buf = self->rdbuf,
	    .size = rc
	};
	Event_raise(self->dataReceived, 0, &rargs);
	return;
    }

    if (rc < 0)
    {
	logmsg(L_WARNING, "server: error reading from connection");
    }
    else
    {
	logmsg(L_INFO, "server: client disconnected");
    }
    Event_raise(self->closed, 0, 0);
}

Connection *Connection_create(int fd)
{
    Connection *self = xmalloc(sizeof *self);
    self->closed = Event_create(self);
    self->dataReceived = Event_create(self);
    self->dataSent = Event_create(self);
    self->fd = fd;
    self->wrbufbusy = 0;
    self->wrbuflen = 0;
    Event_register(Service_readyRead(), self, readConnection, fd);
    Event_register(Service_readyWrite(), self, writeConnection, fd);
    Service_registerRead(fd);
    return self;
}

Event *Connection_closed(Connection *self)
{
    return self->closed;
}

Event *Connection_dataReceived(Connection *self)
{
    return self->dataReceived;
}

Event *Connection_dataSent(Connection *self)
{
    return self->dataSent;
}

int Connection_writeBuffer(Connection *self, char **buf, uint16_t *sz)
{
    if (self->wrbufbusy) return -1;
    if (self->wrbuflen == CONNBUFSZ) return -1;
    *buf = self->wrbuf + self->wrbuflen;
    *sz = CONNBUFSZ - self->wrbuflen;
    self->wrbufbusy = 1;
    return 0;
}

int Connection_commitWrite(Connection *self, uint16_t sz)
{
    if (!self->wrbufbusy) return -1;
    if (self->wrbuflen + sz > CONNBUFSZ) return -1;
    self->wrbuflen += sz;
    Service_registerWrite(self->fd);
    self->wrbufbusy = 0;
    return 0;
}

void Connection_destroy(Connection *self)
{
    if (!self) return;

    Service_unregisterRead(self->fd);
    Service_unregisterWrite(self->fd);
    Event_unregister(Service_readyRead(), self, readConnection, self->fd);
    Event_unregister(Service_readyWrite(), self, writeConnection, self->fd);
    close(self->fd);
    Event_destroy(self->dataSent);
    Event_destroy(self->dataReceived);
    Event_destroy(self->closed);
    free(self);
}

