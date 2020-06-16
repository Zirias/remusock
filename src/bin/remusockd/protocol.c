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

static const Config *cfg;

static Server *sockserver;
static Server *tcpserver;
static Connection *tcpclient;

typedef struct ClientSpec
{
    Connection *sockconn;
    uint16_t clientno;
} ClientSpec;

typedef struct ClientList
{
    ClientSpec *clients;
    uint16_t size;
    uint16_t capa;
} ClientList;

static ClientList *ClientList_create(void)
{
    ClientList *self = xmalloc(sizeof *self);
    self->clients = xmalloc(LISTCHUNK * sizeof *self->clients);
    self->size = 0;
    self->capa = LISTCHUNK;
    return self;
}

static void ClientList_delete(void *list)
{
    if (!list) return;
    ClientList *self = list;
    free(self->clients);
    free(self);
}

static void tcpConnectionLost(void *receiver, const void *sender,
	const void *args)
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

static void busySent(void *receiver, const void *sender,
	const void *args)
{
    (void)receiver;
    (void)sender;
    (void)args;

    Connection *conn = (Connection *)sender;
    Connection_close(conn);
}

static void tcpClientConnected(void *receiver, const void *sender,
	const void *args)
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
	Connection_setData(cca->client, ClientList_create(),
		ClientList_delete);
    }
    else
    {
    }
}

static void tcpClientDisconnected(void *receiver, const void *sender,
	const void *args)
{
    (void)receiver;
    (void)sender;

    const ClientConnectionEventArgs *cca = args;
    if (tcpclient == cca->client) tcpclient = 0;
}

static void sockDataReceived(void *receiver, const void *sender,
	const void *args)
{
    (void)receiver;
    (void)sender;

    const DataReceivedEventArgs *dra = args;
    if (tcpclient)
    {
	Connection_write(tcpclient, dra->buf, dra->size);
    }
}

static void sockClientConnected(void *receiver, const void *sender,
	const void *args)
{
    (void)receiver;
    (void)sender;

    const ClientConnectionEventArgs *cca = args;
    Event_register(Connection_dataReceived(cca->client), 0,
	    sockDataReceived, 0);
}

static void sockClientDisconnected(void *receiver, const void *sender,
	const void *args)
{
    (void)receiver;
    (void)sender;

    const ClientConnectionEventArgs *cca = args;
    Event_unregister(Connection_dataReceived(cca->client), 0,
	    sockDataReceived, 0);
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
	    Connection_setData(tcpclient, ClientList_create(),
		    ClientList_delete);
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

