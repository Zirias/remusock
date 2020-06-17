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
#include <string.h>

#define LISTCHUNK 8
#define PRDBUFSZ 16

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

typedef enum TcpProtoState
{
    TPS_DEFAULT,
    TPS_RDCMD,
    TPS_RDDATA
} TcpProtoState;

typedef struct TcpProtoData
{
    ClientSpec **clients;
    TcpProtoState state;
    uint16_t size;
    uint16_t capa;
    uint16_t rdexpect;
    uint16_t clientno;
    uint8_t rdbufpos;
    char wrbuf[PRDBUFSZ];
    char rdbuf[PRDBUFSZ];
} TcpProtoData;

static TcpProtoData *TcpProtoData_create(void)
{
    TcpProtoData *self = xmalloc(sizeof *self);
    self->clients = xmalloc(LISTCHUNK * sizeof *self->clients);
    self->state = TPS_DEFAULT;
    self->size = 0;
    self->capa = LISTCHUNK;
    self->rdexpect = 0;
    self->rdbufpos = 0;
    return self;
}

static uint16_t registerConnectionAt(Connection *tcpconn,
	Connection *sockconn, uint16_t pos)
{
    TcpProtoData *prdat = Connection_data(tcpconn);
    if (pos < prdat->size && prdat->clients[pos]) return 0xffff;
    if (pos >= prdat->capa)
    {
	while (pos >= prdat->capa)
	{
	    if ((uint16_t)(prdat->capa + LISTCHUNK) < prdat->capa)
	    {
		return 0xffff;
	    }
	    prdat->capa += LISTCHUNK;
	}
	prdat->clients = xrealloc(prdat->clients,
		prdat->capa * sizeof *prdat->clients);
	memset(prdat->clients + prdat->size, 0, prdat->capa - prdat->size);
    }
    ClientSpec *clspec = xmalloc(sizeof *clspec);
    clspec->tcpconn = tcpconn;
    clspec->sockconn = sockconn;
    clspec->clientno = pos;
    prdat->clients[pos] = clspec;
    Connection_setData(sockconn, clspec, 0);
    if (pos >= prdat->size) prdat->size = pos+1;
    return pos;
}

static uint16_t registerConnection(Connection *tcpconn, Connection *sockconn)
{
    TcpProtoData *prdat = Connection_data(tcpconn);
    uint16_t pos;
    for (pos = 0; pos < prdat->size; ++pos)
    {
	if (!prdat->clients[pos]) break;
    }
    return registerConnectionAt(tcpconn, sockconn, pos);
}

static ClientSpec *connectionAt(Connection *tcpconn, uint16_t pos)
{
    TcpProtoData *prdat = Connection_data(tcpconn);
    if (pos >= prdat->size) return 0;
    return prdat->clients[pos];
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

static void sockConnectionLost(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)args;

    Connection *sock = sender;
    ClientSpec *client = Connection_data(sock);
    if (client)
    {
	TcpProtoData *prdat = Connection_data(client->tcpconn);
	prdat->wrbuf[0] = 'b';
	prdat->wrbuf[1] = client->clientno >> 8;
	prdat->wrbuf[2] = client->clientno & 0xff;
	Connection_write(client->tcpconn, prdat->wrbuf, 3, 0);
    }
    if (!sockserver)
    {
	Connection_destroy(sock);
    }
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
	Connection_write(clspec->tcpconn, prdat->wrbuf, 5, 0);
	Connection_write(clspec->tcpconn, dra->buf, dra->size, sockconn);
    }
}

static void tcpConnectionLost(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)args;

    if (tcpclient == sender)
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

static void tcpDataReceived(void *receiver, void *sender, void *args)
{
    (void)receiver;

    Connection *tcpconn = sender;
    DataReceivedEventArgs *dra = args;
    TcpProtoData *prdat = Connection_data(tcpconn);
    uint16_t dpos = 0;
    while (dpos < dra->size)
    {
	switch (prdat->state)
	{
	    case TPS_DEFAULT:
		switch (dra->buf[dpos])
		{
		    case 'h':
			if (sockserver) goto error;
			prdat->rdexpect = 2;
			break;
		    case 'b':
			prdat->rdexpect = 2;
			break;
		    case 'd':
			prdat->rdexpect = 4;
			break;
		    default:
			goto error;
		}
		prdat->rdbuf[prdat->rdbufpos++] = dra->buf[dpos++];
		prdat->state = TPS_RDCMD;
		break;

	    case TPS_RDCMD:
		prdat->rdbuf[prdat->rdbufpos++] = dra->buf[dpos++];
		if (!--prdat->rdexpect)
		{
		    uint16_t clientno =
			(prdat->rdbuf[1] << 8) | prdat->rdbuf[2];
		    Connection *sockclient;
		    ClientSpec *client;
		    switch (prdat->rdbuf[0])
		    {
			case 'h':
			    sockclient = Connection_createUnixClient(cfg);
			    if (sockclient &&
				    registerConnectionAt(tcpconn,
					sockclient, clientno) == clientno)
			    {
				Event_register(
					Connection_dataReceived(sockclient),
					0, sockDataReceived, 0);
				Event_register(
					Connection_closed(sockclient),
					0, sockConnectionLost, 0);
			    }
			    else
			    {
				if (sockclient) Connection_destroy(sockclient);
				prdat->wrbuf[0] = 'b';
				prdat->wrbuf[1] = prdat->rdbuf[1];
				prdat->wrbuf[2] = prdat->rdbuf[2];
				Connection_write(tcpconn, prdat->wrbuf, 3, 0);
			    }
			    prdat->state = TPS_DEFAULT;
			    break;
			case 'b':
			    client = connectionAt(tcpconn, clientno);
			    if (client)
			    {
				sockclient = client->sockconn;
				unregisterConnection(tcpconn, sockclient);
				if (sockserver)
				{
				    Connection_close(sockclient);
				}
				else
				{
				    Connection_destroy(sockclient);
				}
			    }
			    prdat->state = TPS_DEFAULT;
			    break;
			case 'd':
			    prdat->clientno = clientno;
			    prdat->rdexpect =
				(prdat->rdbuf[3] << 8) | prdat->rdbuf[4];
			    prdat->state = TPS_RDDATA;
			    break;
		    }
		    prdat->rdbufpos = 0;
		}
		break;

	    case TPS_RDDATA:
		{
		    uint16_t chunksz = dra->size - dpos;
		    if (chunksz > prdat->rdexpect) chunksz = prdat->rdexpect;
		    ClientSpec *client = connectionAt(
			    tcpconn, prdat->clientno);
		    if (client)
		    {
			Connection_write(client->sockconn, dra->buf + dpos,
				chunksz, 0);
		    }
		    dpos += chunksz;
		    prdat->rdexpect -= chunksz;
		    if (!prdat->rdexpect) prdat->state = TPS_DEFAULT;
		}
		break;
	}
    }
    return;

error:
    Connection_close(tcpconn);
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
    }
    Connection_setData(client, TcpProtoData_create(), TcpProtoData_delete);
    Event_register(Connection_dataReceived(client), 0, tcpDataReceived, 0);
    Event_register(Connection_dataSent(client), 0, tcpDataSent, 0);
}

static void tcpClientDisconnected(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)sender;

    if (tcpclient == args) tcpclient = 0;
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
	Event_register(Connection_dataReceived(tcpclient), 0,
		tcpDataReceived, 0);
	Event_register(Connection_dataSent(tcpclient), 0,
		tcpDataSent, 0);
	Connection_setData(tcpclient, TcpProtoData_create(),
		TcpProtoData_delete);
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

