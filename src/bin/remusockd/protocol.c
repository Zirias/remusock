#include "protocol.h"

#include <poser/core.h>
#include <stdlib.h>
#include <string.h>

#define IDLETICKS 60
#define PINGTICKS 10

const uint8_t idsrv[] = { CMD_IDENT, ARG_SERVER };
const uint8_t idcli[] = { CMD_IDENT, ARG_CLIENT };

static const uint8_t cmdping[] = { CMD_PING };
static const uint8_t cmdpong[] = { CMD_PONG };

typedef enum ProtoSt
{
    PS_CMD,
    PS_CLIENTNO,
    PS_DATAHDR,
    PS_DATA
} ProtoSt;

typedef struct Connection
{
    Protocol *proto;
    PSC_Connection *sockconn;
    uint16_t id;
    uint8_t msgbuf[5];
} Connection;

struct Protocol
{
    PSC_Connection *tcp;
    PSC_Server *sockserver;
    PSC_UnixClientOpts *sockopts;
    PSC_HashTable *connections;
    ProtoSt state;
    int ticks;
    uint16_t nextid;
    uint16_t cmdid;
    uint8_t cmd;
};

static const char *remotestr(PSC_Connection *c);
static const char *key(uint16_t id);
static void deleteconn(void *ptr);
static void sockconnected(void *receiver, void *sender, void *args);
static void sockclosed(void *receiver, void *sender, void *args);
static void sockreceived(void *receiver, void *sender, void *args);
static void socksent(void *receiver, void *sender, void *args);
static void socknewclient(void *receiver, void *sender, void *args);
static int addconnection(Protocol *self, uint16_t id, PSC_Connection *sockconn);
static void sent(void *receiver, void *sender, void *args);
static void received(void *receiver, void *sender, void *args);
static void tick(void *receiver, void *sender, void *args);

static const char *remotestr(PSC_Connection *c)
{
    const char *remAddr = PSC_Connection_remoteAddr(c);
    const char *remHost = PSC_Connection_remoteHost(c);
    if (remAddr && remHost)
    {
	static char buf[1024];
        snprintf(buf, 1024, "%s [%s]", remHost, remAddr);
        return buf;
    }
    if (remHost) return remHost;
    if (remAddr) return remAddr;
    return "<unknown>";
}

static const char *key(uint16_t id)
{
    static char key[sizeof id + 1];
    memcpy(key, &id, sizeof id);
    key[sizeof id] = 0;
    return key;
}

static void deleteconn(void *ptr)
{
    if (!ptr) return;
    Connection *conn = ptr;
    if (conn->sockconn) PSC_Connection_close(conn->sockconn, 0);
    free(conn);
}

static void sockconnected(void *receiver, void *sender, void *args)
{
    (void)sender;
    (void)args;

    Connection *conn = receiver;
    PSC_Log_fmt(PSC_L_DEBUG, "Protocol: connected %s <-> %s",
	    remotestr(conn->sockconn), remotestr(conn->proto->tcp));
    conn->msgbuf[0] = CMD_CONNECT;
    PSC_Connection_sendAsync(conn->proto->tcp, conn->msgbuf, 3, 0);
}

static void sockclosed(void *receiver, void *sender, void *args)
{
    (void)sender;
    (void)args;

    Connection *conn = receiver;
    PSC_Log_fmt(PSC_L_DEBUG, "Protocol: disconnected %s <-> %s",
	    remotestr(conn->sockconn), remotestr(conn->proto->tcp));
    conn->sockconn = 0;
    conn->msgbuf[0] = CMD_BYE;
    PSC_Connection_sendAsync(conn->proto->tcp, conn->msgbuf, 3, conn);
}

static void sockreceived(void *receiver, void *sender, void *args)
{
    (void)sender;

    Connection *conn = receiver;
    PSC_EADataReceived *dra = args;
    size_t sz = PSC_EADataReceived_size(dra);
    conn->msgbuf[0] = CMD_DATA;
    conn->msgbuf[3] = (sz >> 8 & 0xff);
    conn->msgbuf[4] = sz & 0xff;
    PSC_Connection_sendAsync(conn->proto->tcp, conn->msgbuf, 5, 0);
    PSC_Connection_sendAsync(conn->proto->tcp, PSC_EADataReceived_buf(dra),
	    sz, conn);
}

static void socksent(void *receiver, void *sender, void *args)
{
    (void)sender;
    (void)args;

    Connection *conn = receiver;
    PSC_Connection_confirmDataReceived(conn->proto->tcp);
}

static void socknewclient(void *receiver, void *sender, void *args)
{
    (void)sender;

    Protocol *self = receiver;
    PSC_Connection *sockconn = args;

    if (addconnection(self, 0, sockconn) < 0)
    {
	PSC_Log_msg(PSC_L_WARNING,
		"Protocol: error accepting socket connection");
	PSC_Connection_close(sockconn, 0);
    }
}

static int addconnection(Protocol *self, uint16_t id, PSC_Connection *sockconn)
{
    const char *k;
    if (self->sockserver)
    {
	uint16_t first = self->nextid;
	do
	{
	    id = ++self->nextid;
	    if (id == first) return -1;
	    k = key(id);
	} while (PSC_HashTable_get(self->connections, k));
    }
    else
    {
	k = key(id);
	if (PSC_HashTable_get(self->connections, k)) return -1;
    }

    Connection *conn = PSC_malloc(sizeof *conn);
    conn->proto = self;
    conn->id = id;
    conn->msgbuf[1] = id >> 8;
    conn->msgbuf[2] = id & 0xff;

    if (self->sockserver)
    {
	conn->sockconn = sockconn;
	PSC_Connection_pause(sockconn);
	conn->msgbuf[0] = CMD_HELLO;
	PSC_Connection_sendAsync(self->tcp, conn->msgbuf, 3, 0);
    }
    else
    {
	conn->sockconn = PSC_Connection_createUnixClient(self->sockopts);
	if (!conn->sockconn)
	{
	    sockclosed(conn, 0, 0);
	    goto done;
	}
	PSC_Event_register(PSC_Connection_connected(conn->sockconn), conn,
		sockconnected, 0);
    }
    PSC_Event_register(PSC_Connection_closed(conn->sockconn), conn,
	    sockclosed, 0);
    PSC_Event_register(PSC_Connection_dataReceived(conn->sockconn), conn,
	    sockreceived, 0);
    PSC_Event_register(PSC_Connection_dataSent(conn->sockconn), conn,
	    socksent, 0);

done:
    PSC_HashTable_set(self->connections, k, conn, deleteconn);
    return 0;
}

static void sent(void *receiver, void *sender, void *args)
{
    (void)sender;

    Protocol *self = receiver;
    Connection *conn = args;

    if (conn->sockconn)
    {
	PSC_Connection_confirmDataReceived(conn->sockconn);
    }
    else
    {
	PSC_HashTable_delete(self->connections, key(conn->id));
    }
}

static void received(void *receiver, void *sender, void *args)
{
    Protocol *self = receiver;
    PSC_Connection *tcp = sender;
    PSC_EADataReceived *dra = args;

    self->ticks = IDLETICKS;

    const uint8_t *buf = PSC_EADataReceived_buf(dra);
    Connection *conn;

    switch (self->state)
    {
	case PS_CMD:
	    self->cmd = buf[0];
	    switch (self->cmd)
	    {
		case CMD_PING:
		    PSC_Connection_sendAsync(tcp, cmdpong, 1, 0);
		    break;

		case CMD_PONG:
		    break;

		case CMD_HELLO:
		case CMD_CONNECT:
		case CMD_BYE:
		    self->state = PS_CLIENTNO;
		    PSC_Connection_receiveBinary(tcp, 2);
		    break;

		case CMD_DATA:
		    self->state = PS_DATAHDR;
		    PSC_Connection_receiveBinary(tcp, 4);
		    break;

		default:
		    goto error;
	    }
	    break;

	case PS_CLIENTNO:
	    self->cmdid = buf[0] << 8 | buf[1];
	    switch (self->cmd)
	    {
		case CMD_HELLO:
		    if (self->sockserver) goto error;
		    if (addconnection(self, self->cmdid, 0) < 0) goto error;
		    break;

		case CMD_CONNECT:
		    if (!self->sockserver) goto error;
		    conn = PSC_HashTable_get(
			    self->connections, key(self->cmdid));
		    if (!conn) goto error;
		    PSC_Connection_resume(conn->sockconn);
		    PSC_Log_fmt(PSC_L_DEBUG, "Protocol: connected %s <-> %s",
			    remotestr(conn->sockconn), remotestr(tcp));
		    break;

		case CMD_BYE:
		    conn = PSC_HashTable_get(self->connections,
			    key(self->cmdid));
		    if (!conn) goto error;
		    PSC_Event_unregister(PSC_Connection_closed(conn->sockconn),
			    conn, sockclosed, 0);
		    PSC_HashTable_delete(self->connections, key(self->cmdid));
		    PSC_Log_fmt(PSC_L_DEBUG,
			    "Protocol: disconnected %s <-> %s",
			    remotestr(conn->sockconn), remotestr(tcp));
		    break;

		default:
		    goto error;
	    }
	    self->state = PS_CMD;
	    PSC_Connection_receiveBinary(tcp, 1);
	    break;

	case PS_DATAHDR:
	    self->state = PS_DATA;
	    self->cmdid = buf[0] << 8 | buf[1];
	    PSC_Connection_receiveBinary(tcp, buf[2] << 8 | buf[3]);
	    break;

	case PS_DATA:
	    conn = PSC_HashTable_get(self->connections, key(self->cmdid));
	    if (!conn) goto error;
	    PSC_EADataReceived_markHandling(dra);
	    PSC_Connection_sendAsync(conn->sockconn, buf,
		    PSC_EADataReceived_size(dra), conn);
	    self->state = PS_CMD;
	    PSC_Connection_receiveBinary(tcp, 1);
	    break;
    }

    return;

error:
    PSC_Log_fmt(PSC_L_WARNING, "Protocol: unexpected data from %s, "
	    "closing connection", remotestr(self->tcp));
    switch (self->state)
    {
	case PS_CMD:
	    PSC_Log_fmt(PSC_L_DEBUG, "Protocol: unknown command 0x%02hhx",
		    self->cmd);
	    break;

	case PS_CLIENTNO:
	    PSC_Log_fmt(PSC_L_DEBUG, "Protocol: unknown client %u for "
		    "command 0x%02hhx", (int)self->cmdid, self->cmd);
	    break;

	case PS_DATA:
	    PSC_Log_fmt(PSC_L_DEBUG, "Protocol: data for unknwon client %u",
		    (int)self->cmdid);
	    break;

	default:
	    break;
    }
    PSC_Connection_close(self->tcp, 0);
}

static void tick(void *receiver, void *sender, void *args)
{
    (void)sender;
    (void)args;

    Protocol *self = receiver;

    int tickno = --self->ticks;
    if (!tickno)
    {
	PSC_Log_fmt(PSC_L_WARNING, "Protocol: closing unresponsive connection "
		"with %s", remotestr(self->tcp));
	PSC_Connection_close(self->tcp, 0);
    }
    else if (tickno == PINGTICKS)
    {
	PSC_Connection_sendAsync(self->tcp, cmdping, 1, 0);
    }
}

Protocol *Protocol_create(PSC_Connection *tcp, PSC_Server *sockserver,
	PSC_UnixClientOpts *sockopts)
{
    Protocol *self = PSC_malloc(sizeof *self);
    self->tcp = tcp;
    self->sockserver = sockserver;
    self->sockopts = sockopts;
    self->connections = PSC_HashTable_create(6);
    self->state = PS_CMD;
    self->ticks = IDLETICKS;
    self->nextid = 0;
    self->cmd = 0;

    PSC_Event_register(PSC_Service_tick(), self, tick, 0);

    PSC_Event_register(PSC_Connection_dataReceived(tcp), self, received, 0);
    PSC_Event_register(PSC_Connection_dataSent(tcp), self, sent, 0);

    PSC_Connection_receiveBinary(tcp, 1);

    if (sockserver)
    {
	PSC_Event_register(PSC_Server_clientConnected(sockserver), self,
		socknewclient, 0);
	PSC_Server_enable(sockserver);
    }

    PSC_Log_fmt(PSC_L_INFO, "Protocol: connected with %s", remotestr(tcp));

    return self;
}

void Protocol_destroy(Protocol *self)
{
    if (!self) return;

    PSC_Log_fmt(PSC_L_INFO, "Protocol: disconnected from %s",
	    remotestr(self->tcp));

    PSC_HashTableIterator *i = PSC_HashTable_iterator(self->connections);
    while (PSC_HashTableIterator_moveNext(i))
    {
	Connection *conn = PSC_HashTableIterator_current(i);
	PSC_Connection_close(conn->sockconn, 0);
    }
    PSC_HashTableIterator_destroy(i);
    PSC_HashTable_destroy(self->connections);

    if (self->sockserver)
    {
	PSC_Server_disable(self->sockserver);
	PSC_Event_unregister(PSC_Server_clientConnected(self->sockserver),
		self, socknewclient, 0);
    }

    PSC_Event_unregister(PSC_Service_tick(), self, tick, 0);

    free(self);
}

