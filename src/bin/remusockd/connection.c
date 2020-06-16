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
#define NWRITERECS 16

typedef struct WriteRecord
{
    const char *wrbuf;
    uint16_t wrbuflen;
    uint16_t wrbufpos;
} WriteRecord;

typedef struct Connection
{
    Event *closed;
    Event *dataReceived;
    Event *dataSent;
    void *data;
    void (*deleter)(void *);
    WriteRecord writerecs[NWRITERECS];
    DataReceivedEventArgs args;
    int fd;
    uint8_t nrecs;
    uint8_t baserecidx;
    char rdbuf[CONNBUFSZ];
} Connection;

static void writeConnection(void *receiver, void *sender, void *args)
{
    (void)sender;
    (void)args;

    Connection *self = receiver;
    if (!self->nrecs)
    {
	logmsg(L_ERROR, "connection: ready to send with empty buffer");
	Service_unregisterWrite(self->fd);
	return;
    }
    WriteRecord *rec = self->writerecs + self->baserecidx;
    int rc = write(self->fd, rec->wrbuf + rec->wrbufpos,
	    rec->wrbuflen - rec->wrbufpos);
    if (rc > 0)
    {
	if (rc < rec->wrbuflen - rec->wrbufpos)
	{
	    rec->wrbufpos += rc;
	    return;
	}
	if (++self->baserecidx == NWRITERECS) self->baserecidx = 0;
	if (!--self->nrecs)
	{
	    Service_unregisterWrite(self->fd);
	    Event_raise(self->dataSent, 0, 0);
	}
	return;
    }

    logmsg(L_WARNING, "connection: error writing to connection");
    Event_raise(self->closed, 0, 0);
}

static void readConnection(void *receiver, void *sender, void *args)
{
    (void)sender;
    (void)args;

    Connection *self = receiver;
    if (self->args.handling)
    {
	logmsg(L_WARNING,
		"connection: new data while read buffer still handled");
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
	logmsg(L_WARNING, "connection: error reading from connection");
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
    self->data = 0;
    self->deleter = free;
    self->args.buf = self->rdbuf;
    self->args.handling = 0;
    self->nrecs = 0;
    self->baserecidx = 0;
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
    if (self->nrecs == NWRITERECS) return -1;
    WriteRecord *rec = self->writerecs +
	((self->baserecidx + self->nrecs++) % NWRITERECS);
    rec->wrbuflen = sz;
    rec->wrbufpos = 0;
    rec->wrbuf = buf;
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

void Connection_close(Connection *self)
{
    Event_raise(self->closed, 0, 0);
}

void Connection_setData(Connection *self, void *data, void (*deleter)(void *))
{
    self->deleter(self->data);
    self->data = data;
    if (deleter)
    {
	self->deleter = deleter;
    }
    else
    {
	self->deleter = free;
    }
}

void *Connection_data(const Connection *self)
{
    return self->data;
}

void Connection_destroy(Connection *self)
{
    if (!self) return;

    Service_unregisterRead(self->fd);
    Service_unregisterWrite(self->fd);
    Event_unregister(Service_readyRead(), self, readConnection, self->fd);
    Event_unregister(Service_readyWrite(), self, writeConnection, self->fd);
    close(self->fd);
    self->deleter(self->data);
    Event_destroy(self->dataSent);
    Event_destroy(self->dataReceived);
    Event_destroy(self->closed);
    free(self);
}

