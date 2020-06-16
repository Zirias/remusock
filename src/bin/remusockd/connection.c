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
    const char *wrbuf;
    DataReceivedEventArgs args;
    int fd;
    uint16_t wrbuflen;
    uint16_t wrbufpos;
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
    int rc = write(self->fd, self->wrbuf + self->wrbufpos,
	    self->wrbuflen - self->wrbufpos);
    if (rc > 0)
    {
	if (rc < self->wrbuflen - self->wrbufpos)
	{
	    self->wrbufpos += rc;
	    return;
	}
	self->wrbuflen = 0;
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
    if (self->args.handling)
    {
	logmsg(L_WARNING, "server: new data while read buffer still handled");
	return;
    }

    int rc = read(self->fd, self->rdbuf, CONNBUFSZ);
    if (rc > 0)
    {
	self->args.size = rc;
	Event_raise(self->dataReceived, 0, &self->args);
	if (self->args.handling) Service_unregisterRead(self->fd);
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
    self->wrbuflen = 0;
    self->args.buf = self->rdbuf;
    self->args.handling = 0;
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

int Connection_write(Connection *self, const char *buf, uint16_t sz)
{
    if (self->wrbuflen) return -1;
    self->wrbuflen = sz;
    self->wrbufpos = 0;
    self->wrbuf = buf;
    Service_registerWrite(self->fd);
    return 0;
}

int Connection_confirmDataReceived(Connection *self)
{
    if (!self->args.handling) return -1;
    self->args.handling = 0;
    Service_registerRead(self->fd);
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

