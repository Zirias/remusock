#include "client.h"
#include "config.h"
#include "connection.h"
#include "event.h"
#include "eventargs.h"
#include "protocol.h"
#include "server.h"
#include "service.h"
#include "util.h"

#include <stdint.h>
#include <stdlib.h>

#define LISTCHUNK 8
#define PRWRBUFSZ 16

static const Config *cfg;

static Server *sockserver;
static Server *tcpserver;
static Connection *tcpclient;

typedef struct ClientSpec
{
    Connection *tcpconn;
    Connection *sockconn;
    uint16_t clientno;
} ClientSpec;

typedef struct TcpProtoData
{
    ClientSpec **clients;
    uint16_t size;
    uint16_t capa;
    char wrbuf[PRWRBUFSZ];
} TcpProtoData;

static TcpProtoData *TcpProtoData_create(void)
{
    TcpProtoData *self = xmalloc(sizeof *self);
    self->clients = xmalloc(LISTCHUNK * sizeof *self->clients);
    self->size = 0;
    self->capa = LISTCHUNK;
    return self;
}

static uint16_t registerConnection(Connection *tcpconn, Connection *sockconn)
{
    TcpProtoData *prdat = Connection_data(tcpconn);
    uint16_t pos;
    for (pos = 0; pos < prdat->size; ++pos)
    {
	if (!prdat->clients[pos]) break;
    }
    if (pos == prdat->capa)
    {
	if ((uint16_t)(prdat->capa + LISTCHUNK) < prdat->capa) return 0xffff;
	prdat->capa += LISTCHUNK;
	prdat->clients = xrealloc(prdat->clients,
		prdat->capa * sizeof *prdat->clients);
    }
    ClientSpec *clspec = xmalloc(sizeof *clspec);
    clspec->tcpconn = tcpconn;
    clspec->sockconn = sockconn;
    clspec->clientno = pos;
    prdat->clients[pos] = clspec;
    Connection_setData(sockconn, clspec, 0);
    if (pos == prdat->size) ++prdat->size;
    return pos;
}

static void unregisterConnection(Connection *tcpconn, Connection *sockconn)
{
    TcpProtoData *prdat = Connection_data(tcpconn);
    ClientSpec *clspec = Connection_data(sockconn);
    if (prdat && clspec)
    {
	Connection_setData(sockconn, 0, 0);
	prdat->clients[clspec->clientno] = 0;
	free(clspec);
    }
}

static void TcpProtoData_delete(void *list)
{
    if (!list) return;
    TcpProtoData *self = list;
    for (uint16_t pos = 0; pos < self->size; ++pos)
    {
	if (self->clients[pos])
	{
	    unregisterConnection(self->clients[pos]->tcpconn,
		    self->clients[pos]->sockconn);
	}
    }
    free(self->clients);
    free(self);
}

static void tcpDataSent(void *receiver, void *sender, void *args)
{
    (void)sender;
    (void)receiver;

    Connection_confirmDataReceived(args);
}

static void tcpConnectionLost(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)sender;

    if (tcpclient == args)
    {
	Connection_destroy(tcpclient);
	tcpclient = 0;
	Service_quit();
    }
}

static void busySent(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)sender;
    (void)args;

    Connection *conn = sender;
    Connection_close(conn);
}

static void tcpClientConnected(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)sender;

    Connection *client = args;
    if (sockserver)
    {
	if (tcpclient)
	{
	    Event_register(Connection_dataSent(client), 0, busySent, 0);
	    Connection_write(client, "busy.\n", 6, 0);
	    return;
	}
	tcpclient = client;
	Connection_setData(client, TcpProtoData_create(), TcpProtoData_delete);
	Event_register(Connection_dataSent(client), 0, tcpDataSent, 0);
    }
    else
    {
    }
}

static void tcpClientDisconnected(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)sender;

    if (tcpclient == args) tcpclient = 0;
}

static void sockDataReceived(void *receiver, void *sender, void *args)
{
    (void)receiver;

    Connection *sockconn = sender;
    DataReceivedEventArgs *dra = args;
    ClientSpec *clspec = Connection_data(sockconn);
    if (clspec)
    {
	TcpProtoData *prdat = Connection_data(clspec->tcpconn);
	dra->handling = 1;
	prdat->wrbuf[0] = 'd';
	prdat->wrbuf[1] = clspec->clientno >> 8;
	prdat->wrbuf[2] = clspec->clientno & 0xff;
	prdat->wrbuf[3] = dra->size >> 8;
	prdat->wrbuf[4] = dra->size & 0xff;
	Connection_write(tcpclient, prdat->wrbuf, 5, 0);
	Connection_write(tcpclient, dra->buf, dra->size, sockconn);
    }
}

static void sockClientConnected(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)sender;

    Connection *client = args;
    if (!tcpclient)
    {
	Connection_close(client);
	return;
    }
    Event_register(Connection_dataReceived(client), 0, sockDataReceived, 0);
    TcpProtoData *prdat = Connection_data(tcpclient);
    uint16_t clientno = registerConnection(tcpclient, client);
    prdat->wrbuf[0] = 'h';
    prdat->wrbuf[1] = clientno >> 8;
    prdat->wrbuf[2] = clientno & 0xff;
    Connection_write(tcpclient, prdat->wrbuf, 3, 0);
}

static void sockClientDisconnected(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)sender;

    Connection *client = args;
    Event_unregister(Connection_dataReceived(client), 0, sockDataReceived, 0);
    ClientSpec *clspec = Connection_data(client);
    if (clspec)
    {
	TcpProtoData *prdat = Connection_data(clspec->tcpconn);
	prdat->wrbuf[0] = 'b';
	prdat->wrbuf[1] = clspec->clientno >> 8;
	prdat->wrbuf[2] = clspec->clientno & 0xff;
	Connection_write(tcpclient, prdat->wrbuf, 3, 0);
	unregisterConnection(clspec->tcpconn, client);
    }
}

int Protocol_init(const Config *config)
{
    if (cfg) return -1;
    cfg = config;
    if (!config->sockClient)
    {
	sockserver = Server_createUnix(config);
	if (!sockserver) return -1;
	Event_register(Server_clientConnected(sockserver), 0,
		sockClientConnected, 0);
	Event_register(Server_clientDisconnected(sockserver), 0,
		sockClientDisconnected, 0);
    }
    if (config->remotehost)
    {
	tcpclient = Connection_createTcpClient(config);
	if (!tcpclient)
	{
	    Server_destroy(sockserver);
	    sockserver = 0;
	    return -1;
	}
	Event_register(Connection_closed(tcpclient), 0,
		tcpConnectionLost, 0);
	if (sockserver)
	{
	    Connection_setData(tcpclient, TcpProtoData_create(),
		    TcpProtoData_delete);
	}
    }
    else
    {
	tcpserver = Server_createTcp(config);
	if (!tcpserver)
	{
	    Server_destroy(sockserver);
	    sockserver = 0;
	    return -1;
	}
	Event_register(Server_clientConnected(tcpserver), 0,
		tcpClientConnected, 0);
	Event_register(Server_clientDisconnected(tcpserver), 0,
		tcpClientDisconnected, 0);
    }
    return 0;
}

int Protocol_done(void)
{
    if (!cfg) return -1;
    if (tcpclient) Connection_close(tcpclient);
    Server_destroy(sockserver);
    Server_destroy(tcpserver);
    tcpclient = 0;
    sockserver = 0;
    tcpserver = 0;
    cfg = 0;
    return 0;
}

