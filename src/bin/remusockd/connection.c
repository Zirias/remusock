#define _DEFAULT_SOURCE

#include "connection.h"
#include "event.h"
#include "log.h"
#include "service.h"
#include "threadpool.h"
#include "util.h"

#include <errno.h>
#include <netdb.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#define CONNBUFSZ 4096
#define NWRITERECS 16
#define CONNTICKS 6
#define RESOLVTICKS 6

static char hostbuf[NI_MAXHOST];
static char servbuf[NI_MAXSERV];

typedef struct WriteRecord
{
    const uint8_t *wrbuf;
    void *id;
    uint16_t wrbuflen;
    uint16_t wrbufpos;
} WriteRecord;

typedef struct RemoteAddrResolveArgs
{
    union {
	struct sockaddr sa;
	struct sockaddr_storage ss;
    };
    socklen_t addrlen;
    int rc;
    char name[NI_MAXHOST];
} RemoteAddrResolveArgs;

typedef struct Connection
{
    Event *connected;
    Event *closed;
    Event *dataReceived;
    Event *dataSent;
    ThreadJob *resolveJob;
    char *addr;
    char *name;
    void *data;
    void (*deleter)(void *);
    WriteRecord writerecs[NWRITERECS];
    DataReceivedEventArgs args;
    RemoteAddrResolveArgs resolveArgs;
    int fd;
    int connecting;
    uint8_t deleteScheduled;
    uint8_t nrecs;
    uint8_t baserecidx;
    uint8_t readOffset;
    uint8_t rdbuf[CONNBUFSZ];
} Connection;

static void deleteLater(Connection *self);

static void checkPendingConnection(void *receiver, void *sender, void *args)
{
    (void)sender;
    (void)args;

    Connection *self = receiver;
    if (self->connecting && !--self->connecting)
    {
	logfmt(L_INFO, "connection: timeout connecting to %s",
		Connection_remoteAddr(self));
	Service_unregisterWrite(self->fd);
	Connection_close(self);
    }
}

static void writeConnection(void *receiver, void *sender, void *args)
{
    (void)sender;
    (void)args;

    Connection *self = receiver;
    if (self->connecting)
    {
	Event_unregister(Service_tick(), self, checkPendingConnection, 0);
	int err = 0;
	socklen_t errlen = sizeof err;
	if (getsockopt(self->fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0
		|| err)
	{
	    logfmt(L_INFO, "connection: failed to connect to %s",
		    Connection_remoteAddr(self));
	    Connection_close(self);
	    return;
	}
	self->connecting = 0;
	Service_registerRead(self->fd);
	if (!self->nrecs) Service_unregisterWrite(self->fd);
	logfmt(L_DEBUG, "connection: connected to %s",
		Connection_remoteAddr(self));
	Event_raise(self->connected, 0, 0);
	return;
    }
    logfmt(L_DEBUG, "connection: ready to write to %s",
	Connection_remoteAddr(self));
    if (!self->nrecs)
    {
	logfmt(L_ERROR, "connection: ready to send to %s with empty buffer",
		Connection_remoteAddr(self));
	Service_unregisterWrite(self->fd);
	return;
    }
    WriteRecord *rec = self->writerecs + self->baserecidx;
    errno = 0;
    int rc = write(self->fd, rec->wrbuf + rec->wrbufpos,
	    rec->wrbuflen - rec->wrbufpos);
    void *id = 0;
    if (rc >= 0)
    {
	if (rc < rec->wrbuflen - rec->wrbufpos)
	{
	    rec->wrbufpos += rc;
	    return;
	}
	else id = rec->id;
	if (++self->baserecidx == NWRITERECS) self->baserecidx = 0;
	if (!--self->nrecs)
	{
	    Service_unregisterWrite(self->fd);
	}
	if (id)
	{
	    Event_raise(self->dataSent, 0, id);
	}
    }
    else if (errno == EWOULDBLOCK || errno == EAGAIN)
    {
	logfmt(L_INFO, "connection: not ready for writing to %s",
		Connection_remoteAddr(self));
	return;
    }
    else
    {
	logfmt(L_WARNING, "connection: error writing to %s",
		Connection_remoteAddr(self));
	Connection_close(self);
    }
}

static void readConnection(void *receiver, void *sender, void *args)
{
    (void)sender;
    (void)args;

    Connection *self = receiver;
    logfmt(L_DEBUG, "connection: ready to read from %s",
	    Connection_remoteAddr(self));
    if (self->args.handling)
    {
	logfmt(L_WARNING,
		"connection: new data while read buffer from %s still handled",
		Connection_remoteAddr(self));
	return;
    }

    errno = 0;
    int rc = read(self->fd, self->rdbuf + self->readOffset,
	    CONNBUFSZ - self->readOffset);
    if (rc > 0)
    {
	self->args.size = rc;
	self->args.offset = self->readOffset;
	Event_raise(self->dataReceived, 0, &self->args);
	if (self->args.handling)
	{
	    logfmt(L_DEBUG, "connection: blocking reads from %s",
		    Connection_remoteAddr(self));
	    Service_unregisterRead(self->fd);
	}
    }
    else if (errno == EWOULDBLOCK || errno == EAGAIN)
    {
	logfmt(L_INFO, "connection: ignoring spurious read from %s",
		Connection_remoteAddr(self));
    }
    else
    {
	if (rc < 0)
	{
	    logfmt(L_WARNING, "connection: error reading from %s",
		    Connection_remoteAddr(self));
	}
	Connection_close(self);
    }
}

static void deleteConnection(void *receiver, void *sender, void *args)
{
    (void)sender;
    (void)args;

    Connection *self = receiver;
    self->deleteScheduled = 2;
    Connection_destroy(self);
}

Connection *Connection_create(int fd, ConnectionCreateMode mode,
	uint8_t readOffset)
{
    Connection *self = xmalloc(sizeof *self);
    self->connected = Event_create(self);
    self->closed = Event_create(self);
    self->dataReceived = Event_create(self);
    self->dataSent = Event_create(self);
    self->resolveJob = 0;
    self->fd = fd;
    self->connecting = 0;
    self->addr = 0;
    self->name = 0;
    self->data = 0;
    self->deleter = 0;
    self->args.buf = self->rdbuf;
    self->args.handling = 0;
    self->deleteScheduled = 0;
    self->nrecs = 0;
    self->baserecidx = 0;
    self->readOffset = readOffset;
    Event_register(Service_readyRead(), self, readConnection, fd);
    Event_register(Service_readyWrite(), self, writeConnection, fd);
    if (mode == CCM_CONNECTING)
    {
	self->connecting = CONNTICKS;
	Event_register(Service_tick(), self, checkPendingConnection, 0);
	Service_registerWrite(fd);
    }
    else if (mode == CCM_NORMAL)
    {
	Service_registerRead(fd);
    }
    return self;
}

Event *Connection_connected(Connection *self)
{
    return self->connected;
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

const char *Connection_remoteAddr(const Connection *self)
{
    if (!self->addr) return "<unknown>";
    return self->addr;
}

const char *Connection_remoteHost(const Connection *self)
{
    return self->name;
}

static void resolveRemoteAddrProc(void *arg)
{
    RemoteAddrResolveArgs *rara = arg;
    char buf[NI_MAXSERV];
    rara->rc = getnameinfo(&rara->sa, rara->addrlen,
	    rara->name, sizeof rara->name, buf, sizeof buf, NI_NUMERICSERV);
}

static void resolveRemoteAddrFinished(void *receiver, void *sender, void *args)
{
    Connection *self = receiver;
    ThreadJob *job = sender;
    RemoteAddrResolveArgs *rara = args;

    if (ThreadJob_hasCompleted(job))
    {
	if (rara->rc >= 0 && strcmp(rara->name, self->addr) != 0)
	{
	    logfmt(L_DEBUG, "connection: %s is %s", self->addr, rara->name);
	    self->name = copystr(rara->name);
	}
	else
	{
	    logfmt(L_DEBUG, "connection: error resolving name for %s",
		    self->addr);
	}
    }
    else
    {
	logfmt(L_DEBUG, "connection: timeout resolving name for %s",
		self->addr);
    }
    self->resolveJob = 0;
}

void Connection_setRemoteAddr(Connection *self,
	struct sockaddr *addr, socklen_t addrlen, int numericOnly)
{
    free(self->addr);
    free(self->name);
    self->addr = 0;
    self->name = 0;
    if (getnameinfo(addr, addrlen, hostbuf, sizeof hostbuf,
		servbuf, sizeof servbuf, NI_NUMERICHOST|NI_NUMERICSERV) >= 0)
    {
	self->addr = copystr(hostbuf);
	if (!numericOnly && !self->resolveJob && ThreadPool_active())
	{
	    memcpy(&self->resolveArgs.sa, addr, addrlen);
	    self->resolveArgs.addrlen = addrlen;
	    self->resolveJob = ThreadJob_create(resolveRemoteAddrProc,
		    &self->resolveArgs, RESOLVTICKS);
	    Event_register(ThreadJob_finished(self->resolveJob), self,
		    resolveRemoteAddrFinished, 0);
	    ThreadPool_enqueue(self->resolveJob);
	}
    }
}

void Connection_setRemoteAddrStr(Connection *self, const char *addr)
{
    free(self->addr);
    free(self->name);
    self->addr = copystr(addr);
    self->name = 0;
}

int Connection_write(Connection *self,
	const uint8_t *buf, uint16_t sz, void *id)
{
    if (self->nrecs == NWRITERECS) return -1;
    WriteRecord *rec = self->writerecs +
	((self->baserecidx + self->nrecs++) % NWRITERECS);
    rec->wrbuflen = sz;
    rec->wrbufpos = 0;
    rec->wrbuf = buf;
    rec->id = id;
    Service_registerWrite(self->fd);
    return 0;
}

void Connection_activate(Connection *self)
{
    if (self->args.handling) return;
    logfmt(L_DEBUG, "connection: unblocking reads from %s",
	    Connection_remoteAddr(self));
    Service_registerRead(self->fd);
}

int Connection_confirmDataReceived(Connection *self)
{
    if (!self->args.handling) return -1;
    self->args.handling = 0;
    Connection_activate(self);
    return 0;
}

void Connection_close(Connection *self)
{
    Event_raise(self->closed, 0, 0);
    deleteLater(self);
}

void Connection_setData(Connection *self, void *data, void (*deleter)(void *))
{
    if (self->deleter) self->deleter(self->data);
    self->data = data;
    self->deleter = deleter;
}

void *Connection_data(const Connection *self)
{
    return self->data;
}

static void deleteLater(Connection *self)
{
    if (!self) return;
    if (!self->deleteScheduled)
    {
	close(self->fd);
	Event_register(Service_eventsDone(), self, deleteConnection, 0);
	self->deleteScheduled = 1;
    }
}

void Connection_destroy(Connection *self)
{
    if (!self) return;
    if (self->deleteScheduled == 1) return;

    Service_unregisterRead(self->fd);
    Service_unregisterWrite(self->fd);
    for (; self->nrecs; --self->nrecs)
    {
	WriteRecord *rec = self->writerecs + self->baserecidx;
	if (rec->id)
	{
	    Event_raise(self->dataSent, 0, rec->id);
	}
	if (++self->baserecidx == NWRITERECS) self->baserecidx = 0;
    }
    if (self->deleteScheduled)
    {
	Event_unregister(Service_eventsDone(), self, deleteConnection, 0);
    }
    else
    {
	close(self->fd);
    }
    Event_unregister(Service_tick(), self, checkPendingConnection, 0);
    Event_unregister(Service_readyRead(), self, readConnection, self->fd);
    Event_unregister(Service_readyWrite(), self, writeConnection, self->fd);
    if (self->resolveJob)
    {
	ThreadPool_cancel(self->resolveJob);
	Event_unregister(ThreadJob_finished(self->resolveJob), self,
		resolveRemoteAddrFinished, 0);
    }
    if (self->deleter) self->deleter(self->data);
    free(self->addr);
    free(self->name);
    Event_destroy(self->dataSent);
    Event_destroy(self->dataReceived);
    Event_destroy(self->closed);
    Event_destroy(self->connected);
    free(self);
}

