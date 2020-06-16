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
    Connection *sockconn;
    uint16_t clientno;
} ClientSpec;

typedef struct TcpProtoData
{
    ClientSpec *clients;
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

static void TcpProtoData_delete(void *list)
{
    if (!list) return;
    TcpProtoData *self = list;
    free(self->clients);
    free(self);
}

static uint16_t TcpProtoData_addClient(TcpProtoData *self,
	Connection *sockconn)
{
    uint16_t pos;
    for (pos = 0; pos < self->size; ++pos)
    {
	if (!self->clients[pos].sockconn) break;
    }
    if (pos == self->capa)
    {
	if ((uint16_t)(self->capa + LISTCHUNK) < self->capa) return 0xffff;
	self->capa += LISTCHUNK;
	self->clients = xrealloc(self->clients,
		self->capa * sizeof *self->clients);
    }
    self->clients[pos].sockconn = sockconn;
    self->clients[pos].clientno = 0xffff;
    if (pos == self->size) ++self->size;
    return pos;
}

static ClientSpec *TcpProtoData_findClient(TcpProtoData *self,
	const Connection *sockconn, uint16_t *clientno)
{
    for (uint16_t pos = 0; pos < self->size; ++pos)
    {
	if (self->clients[pos].sockconn == sockconn)
	{
	    if (clientno) *clientno = pos;
	    return self->clients + pos;
	}
    }
    return 0;
}

static void tcpDataSent(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)args;

    const Connection *tcpconn = sender;
    TcpProtoData *prdat = Connection_data(tcpconn);
    if (prdat)
    {
	for (uint16_t pos = 0; pos < prdat->size; ++pos)
	{
	    if (prdat->clients[pos].sockconn)
	    {
		Connection_confirmDataReceived(prdat->clients[pos].sockconn);
	    }
	}
    }
}

static void tcpConnectionLost(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)sender;

    const ClientConnectionEventArgs *cca = args;
    if (tcpclient == cca->client)
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

    const ClientConnectionEventArgs *cca = args;
    if (sockserver)
    {
	if (tcpclient)
	{
	    Event_register(Connection_dataSent(cca->client), 0, busySent, 0);
	    Connection_write(cca->client, "busy.\n", 6);
	    return;
	}
	tcpclient = cca->client;
	Connection_setData(cca->client, TcpProtoData_create(),
		TcpProtoData_delete);
	Event_register(Connection_dataSent(cca->client), 0, tcpDataSent, 0);
    }
    else
    {
    }
}

static void tcpClientDisconnected(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)sender;

    const ClientConnectionEventArgs *cca = args;
    if (tcpclient == cca->client) tcpclient = 0;
}

static void sockDataReceived(void *receiver, void *sender, void *args)
{
    (void)receiver;

    const Connection *sockconn = sender;
    DataReceivedEventArgs *dra = args;
    if (tcpclient)
    {
	TcpProtoData *prdat = Connection_data(tcpclient);
	uint16_t clientno;
	ClientSpec *client = TcpProtoData_findClient(
		prdat, sockconn, &clientno);
	if (client)
	{
	    dra->handling = 1;
	    prdat->wrbuf[0] = 'd';
	    prdat->wrbuf[1] = clientno >> 8;
	    prdat->wrbuf[2] = clientno & 0xff;
	    prdat->wrbuf[3] = dra->size >> 8;
	    prdat->wrbuf[4] = dra->size & 0xff;
	    Connection_write(tcpclient, prdat->wrbuf, 5);
	    Connection_write(tcpclient, dra->buf, dra->size);
	}
    }
}

static void sockClientConnected(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)sender;

    const ClientConnectionEventArgs *cca = args;
    if (!tcpclient)
    {
	Connection_close(cca->client);
	return;
    }
    Event_register(Connection_dataReceived(cca->client), 0,
	    sockDataReceived, 0);
    TcpProtoData *prdat = Connection_data(tcpclient);
    uint16_t clientno = TcpProtoData_addClient(prdat, cca->client);
    prdat->wrbuf[0] = 'h';
    prdat->wrbuf[1] = clientno >> 8;
    prdat->wrbuf[2] = clientno & 0xff;
    Connection_write(tcpclient, prdat->wrbuf, 3);
}

static void sockClientDisconnected(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)sender;

    const ClientConnectionEventArgs *cca = args;
    Event_unregister(Connection_dataReceived(cca->client), 0,
	    sockDataReceived, 0);
    if (tcpclient)
    {
	TcpProtoData *prdat = Connection_data(tcpclient);
	uint16_t clientno;
	ClientSpec *client = TcpProtoData_findClient(
		prdat, cca->client, &clientno);
	if (client)
	{
	    client->sockconn = 0;
	    prdat->wrbuf[0] = 'b';
	    prdat->wrbuf[1] = clientno >> 8;
	    prdat->wrbuf[2] = clientno & 0xff;
	    Connection_write(tcpclient, prdat->wrbuf, 3);
	}
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

