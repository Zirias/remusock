#include "client.h"
#include "config.h"
#include "connection.h"
#include "event.h"
#include "eventargs.h"
#include "log.h"
#include "protocol.h"
#include "server.h"
#include "service.h"
#include "util.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define LISTCHUNK 8
#define PRDBUFSZ 6

#define CMD_IDENT   0x49
#define CMD_PING    0x3f
#define CMD_PONG    0x21
#define CMD_HELLO   0x48
#define CMD_CONNECT 0x43
#define CMD_BYE	    0x42
#define	CMD_DATA    0x44

#define ARG_SERVER  0x53
#define ARG_CLIENT  0x43

#define IDENTTICKS 2
#define RECONNTICKS 6
#define PINGTICKS 18
#define CLOSETICKS 20

static const Config *cfg;

static Server *sockserver;
static Server *tcpserver;
static Connection *tcpclient;
static Connection *pendingtcp;
static int reconnWait;
static int reconnTicking;

typedef struct ClientSpec
{
    Connection *tcpconn;
    Connection *sockconn;
    uint16_t clientno;
} ClientSpec;

typedef enum TcpProtoState
{
    TPS_IDENT,
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
    uint8_t idleTicks;
    uint8_t nwriteconns;
    uint8_t rdbufpos;
    char wrbuf[PRDBUFSZ];
    char rdbuf[PRDBUFSZ];
} TcpProtoData;

static TcpProtoData *TcpProtoData_create(void)
{
    TcpProtoData *self = xmalloc(sizeof *self);
    self->clients = xmalloc(LISTCHUNK * sizeof *self->clients);
    self->state = TPS_IDENT;
    self->size = 0;
    self->capa = LISTCHUNK;
    self->rdexpect = 0;
    self->idleTicks = 0;
    self->nwriteconns = 0;
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
	    Connection *sockconn = self->clients[pos]->sockconn;
	    unregisterConnection(self->clients[pos]->tcpconn, sockconn);
	    Connection_close(sockconn);
	}
    }
    free(self->clients);
    free(self);
}

static void sockDataSent(void *receiver, void *sender, void *args)
{
    (void)sender;
    (void)receiver;

    Connection *tcpconn = args;
    TcpProtoData *prdat = Connection_data(tcpconn);
    if (!--prdat->nwriteconns)
    {
	Connection_confirmDataReceived(tcpconn);
    }
}

static void tcpDataSent(void *receiver, void *sender, void *args)
{
    (void)sender;
    (void)receiver;

    Connection_confirmDataReceived(args);
}

static void tcpTick(void *receiver, void *sender, void *args)
{
    (void)sender;
    (void)args;

    Connection *tcpconn = receiver;
    TcpProtoData *prdat = Connection_data(tcpconn);
    uint8_t ticks = ++prdat->idleTicks;
    if (prdat->state == TPS_IDENT && ticks == IDENTTICKS)
    {
	logfmt(L_INFO, "protocol: timeout waiting for ident from %s",
		Connection_remoteAddr(tcpconn));
	Connection_close(tcpconn);
    }
    else if (ticks == CLOSETICKS)
    {
	logfmt(L_INFO, "protocol: closing unresponsive connection to %s",
		Connection_remoteAddr(tcpconn));
	Connection_close(tcpconn);
    }
    else if (ticks == PINGTICKS)
    {
	logfmt(L_DEBUG, "protocol: pinging idle connection to %s",
		Connection_remoteAddr(tcpconn));
	prdat->wrbuf[0] = CMD_PING;
	Connection_write(tcpconn, prdat->wrbuf, 1, 0);
    }
}

static void sockConnectionLost(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)args;

    Connection *sock = sender;
    ClientSpec *client = Connection_data(sock);
    if (client && client->tcpconn)
    {
	TcpProtoData *prdat = Connection_data(client->tcpconn);
	prdat->wrbuf[0] = CMD_BYE;
	prdat->wrbuf[1] = client->clientno >> 8;
	prdat->wrbuf[2] = client->clientno & 0xff;
	Connection_write(client->tcpconn, prdat->wrbuf, 3, 0);
	unregisterConnection(client->tcpconn, sock);
    }
    Connection_deleteLater(sock);
}

static void sockConnectionEstablished(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)args;

    Connection *sockconn = sender;
    ClientSpec *clspec = Connection_data(sockconn);
    if (clspec && clspec->tcpconn)
    {
	TcpProtoData *prdat = Connection_data(clspec->tcpconn);
	prdat->wrbuf[0] = CMD_CONNECT;
	prdat->wrbuf[1] = clspec->clientno >> 8;
	prdat->wrbuf[2] = clspec->clientno & 0xff;
	Connection_write(clspec->tcpconn, prdat->wrbuf, 3, 0);
    }
}

static void sockDataReceived(void *receiver, void *sender, void *args)
{
    (void)receiver;

    Connection *sockconn = sender;
    DataReceivedEventArgs *dra = args;
    ClientSpec *clspec = Connection_data(sockconn);
    if (clspec && clspec->tcpconn)
    {
	dra->handling = 1;
	dra->buf[0] = CMD_DATA;
	dra->buf[1] = clspec->clientno >> 8;
	dra->buf[2] = clspec->clientno & 0xff;
	dra->buf[3] = dra->size >> 8;
	dra->buf[4] = dra->size & 0xff;
	Connection_write(clspec->tcpconn, dra->buf, dra->size + 5, sockconn);
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
    logfmt(L_DEBUG, "protocol: received TCP data from %s in local state %d",
	    Connection_remoteAddr(tcpconn), prdat->state);
    prdat->idleTicks = 0;
    uint16_t dpos = 0;
    while (dpos < dra->size)
    {
	switch (prdat->state)
	{
	    case TPS_IDENT:
		if (dra->buf[dpos] != CMD_IDENT
			|| ++dpos == dra->size) goto error;
		if (dra->buf[dpos] != ARG_SERVER
			&& dra->buf[dpos] != ARG_CLIENT) goto error;
		if (sockserver && dra->buf[dpos] == ARG_SERVER)
		{
		    logfmt(L_INFO, "protocol: "
			    "dropping connection to other socket server at %s",
			    Connection_remoteAddr(tcpconn));
		    goto error;
		}
		if (!sockserver && dra->buf[dpos] == ARG_CLIENT)
		{
		    logfmt(L_INFO, "protocol: "
			    "dropping connection to other socket client at %s",
			    Connection_remoteAddr(tcpconn));
		    goto error;
		}
		++dpos;
		prdat->state = TPS_DEFAULT;
		if (!tcpserver)
		{
		    prdat->wrbuf[0] = CMD_IDENT;
		    prdat->wrbuf[1] = sockserver ? ARG_SERVER : ARG_CLIENT;
		    Connection_write(tcpconn, prdat->wrbuf, 2, 0);
		}
		break;

	    case TPS_DEFAULT:
		switch (dra->buf[dpos])
		{
		    case CMD_PING:
			prdat->wrbuf[0] = CMD_PONG;
			Connection_write(tcpconn, prdat->wrbuf, 1, 0);
			++dpos;
			continue;
		    case CMD_PONG:
			++dpos;
			continue;
		    case CMD_HELLO:
			if (sockserver) goto error;
			prdat->rdexpect = 2;
			break;
		    case CMD_CONNECT:
			if (!sockserver) goto error;
			prdat->rdexpect = 2;
			break;
		    case CMD_BYE:
			prdat->rdexpect = 2;
			break;
		    case CMD_DATA:
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
			case CMD_HELLO:
			    sockclient = Connection_createUnixClient(cfg, 5);
			    if (sockclient &&
				    registerConnectionAt(tcpconn,
					sockclient, clientno) == clientno)
			    {
				Event_register(
					Connection_connected(sockclient),
					0, sockConnectionEstablished, 0);
				Event_register(
					Connection_dataReceived(sockclient),
					0, sockDataReceived, 0);
				Event_register(
					Connection_dataSent(sockclient),
					0, sockDataSent, 0);
				Event_register(
					Connection_closed(sockclient),
					0, sockConnectionLost, 0);
				logfmt(L_INFO, "protocol: new remote socket "
					"client from %s on %s",
					Connection_remoteAddr(tcpconn),
					Connection_remoteAddr(sockclient));
			    }
			    else
			    {
				logfmt(L_WARNING, "protocol: cannot connect "
					"new remote socket client from %s",
					Connection_remoteAddr(tcpconn));
				if (sockclient) Connection_destroy(sockclient);
				prdat->wrbuf[0] = CMD_BYE;
				prdat->wrbuf[1] = prdat->rdbuf[1];
				prdat->wrbuf[2] = prdat->rdbuf[2];
				Connection_write(tcpconn, prdat->wrbuf, 3, 0);
			    }
			    prdat->state = TPS_DEFAULT;
			    break;
			case CMD_CONNECT:
			    client = connectionAt(tcpconn, clientno);
			    if (!client)
			    {
				logfmt(L_INFO, "protocol: ignored accepted "
					"socket client from %s "
					"(already closed)",
					Connection_remoteAddr(tcpconn));
				break;
			    }
			    sockclient = client->sockconn;
			    logfmt(L_INFO, "protocol: remote socket client "
				    "accepted from %s",
				    Connection_remoteAddr(tcpconn));
			    Connection_activate(sockclient);
			    prdat->state = TPS_DEFAULT;
			    break;
			case CMD_BYE:
			    client = connectionAt(tcpconn, clientno);
			    if (!client)
			    {
				logfmt(L_INFO, "protocol: ignored closed "
					"socket client from %s "
					"(already closed)",
					Connection_remoteAddr(tcpconn));
				break;
			    }
			    sockclient = client->sockconn;
			    logfmt(L_INFO, "protocol: remote socket client "
				    "from %s disconnected on %s",
				    Connection_remoteAddr(tcpconn),
				    Connection_remoteAddr(sockclient));
			    unregisterConnection(tcpconn, sockclient);
			    if (sockserver)
			    {
				Connection_close(sockclient);
			    }
			    else
			    {
				Connection_destroy(sockclient);
			    }
			    prdat->state = TPS_DEFAULT;
			    break;
			case CMD_DATA:
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
			++prdat->nwriteconns;
			dra->handling = 1;
			Connection_write(client->sockconn, dra->buf + dpos,
				chunksz, tcpconn);
		    }
		    dpos += chunksz;
		    prdat->rdexpect -= chunksz;
		    if (!prdat->rdexpect)
		    {
			prdat->state = TPS_DEFAULT;
			if (!client)
			{
			    logfmt(L_INFO, "protocol: ignored data from %s "
				    "for closed socket",
				    Connection_remoteAddr(tcpconn));
			}
		    }
		}
		break;
	}
    }
    return;

error:
    logfmt(L_INFO, "protocol: protocol error from %s",
	    Connection_remoteAddr(tcpconn));
    Connection_close(tcpconn);
}

static void tcpReconnect(void *receiver, void *sender, void *args);

static void disableReconnTick(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)sender;
    (void)args;

    if (reconnTicking && pendingtcp)
    {
	Event_unregister(Service_tick(), 0, tcpReconnect, 0);
	reconnTicking = 0;
    }
}

static void tcpConnectionLost(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)args;

    Connection *conn = sender;
    if (conn != pendingtcp)
    {
	Event_unregister(Service_tick(), conn, tcpTick, 0);
	logfmt(L_INFO, "protocol: lost TCP connection to %s",
		Connection_remoteAddr(conn));
    }
    if (conn == tcpclient || conn == pendingtcp)
    {
	if (conn == tcpclient)
	{
	    tcpclient = 0;
	}
	if (!tcpserver)
	{
	    logmsg(L_DEBUG, "protocol: scheduling TCP reconnect");
	    Event_register(Service_tick(), 0, tcpReconnect, 0);
	    reconnTicking = 1;
	    reconnWait = pendingtcp ? RECONNTICKS : 1;
	    pendingtcp = 0;
	}
    }
    Connection_deleteLater(conn);
}

static void tcpConnectionEstablished(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)args;

    if (tcpclient || sender != pendingtcp)
    {
	logmsg(L_FATAL, "protocol: unexpected TCP connection");
	Service_quit();
    }
    tcpclient = sender;
    pendingtcp = 0;
    Event_register(Connection_dataReceived(tcpclient), 0, tcpDataReceived, 0);
    Event_register(Connection_dataSent(tcpclient), 0, tcpDataSent, 0);
    Event_register(Service_tick(), tcpclient, tcpTick, 0);
    Connection_setData(tcpclient, TcpProtoData_create(), TcpProtoData_delete);
    logfmt(L_INFO, "protocol: TCP connection established to %s",
	    Connection_remoteAddr(tcpclient));
}

static void tcpReconnect(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)sender;
    (void)args;

    if (!--reconnWait)
    {
	logmsg(L_DEBUG, "protocol: attempting to reconnect TCP");
	pendingtcp = Connection_createTcpClient(cfg, 0);
	if (!pendingtcp)
	{
	    reconnWait = RECONNTICKS;
	    return;
	}
	Event_register(Connection_connected(pendingtcp), 0,
		tcpConnectionEstablished, 0);
	Event_register(Connection_closed(pendingtcp), 0,
		tcpConnectionLost, 0);
    }
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
	    logfmt(L_DEBUG, "protocol: rejecting second TCP connection from "
		    "%s to socket server", Connection_remoteAddr(client));
	    Event_register(Connection_dataSent(client), 0, busySent, 0);
	    Connection_write(client, "busy.\n", 6, client);
	    return;
	}
	tcpclient = client;
    }
    TcpProtoData *prdat = TcpProtoData_create();
    Connection_setData(client, prdat, TcpProtoData_delete);
    Event_register(Connection_closed(client), 0, tcpConnectionLost, 0);
    Event_register(Connection_dataReceived(client), 0, tcpDataReceived, 0);
    Event_register(Connection_dataSent(client), 0, tcpDataSent, 0);
    logfmt(L_INFO, "protocol: TCP client connected from %s",
	    Connection_remoteAddr(client));
    Event_register(Service_tick(), client, tcpTick, 0);
    prdat->wrbuf[0] = CMD_IDENT;
    prdat->wrbuf[1] = sockserver ? ARG_SERVER : ARG_CLIENT;
    Connection_write(client, prdat->wrbuf, 2, 0);
}

static void sockClientConnected(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)sender;

    Connection *client = args;
    logfmt(L_INFO, "protocol: new socket client on %s",
	    Connection_remoteAddr(client));
    if (!tcpclient)
    {
	Connection_close(client);
	return;
    }
    Event_register(Connection_dataReceived(client), 0, sockDataReceived, 0);
    Event_register(Connection_dataSent(client), 0, sockDataSent, 0);
    TcpProtoData *prdat = Connection_data(tcpclient);
    uint16_t clientno = registerConnection(tcpclient, client);
    prdat->wrbuf[0] = CMD_HELLO;
    prdat->wrbuf[1] = clientno >> 8;
    prdat->wrbuf[2] = clientno & 0xff;
    Connection_write(tcpclient, prdat->wrbuf, 3, 0);
}

static void sockClientDisconnected(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)sender;

    Connection *client = args;
    ClientSpec *clspec = Connection_data(client);
    logfmt(L_INFO, "protocol: socket client disconnected on %s",
	    Connection_remoteAddr(client));
    if (clspec && clspec->tcpconn)
    {
	TcpProtoData *prdat = Connection_data(clspec->tcpconn);
	prdat->wrbuf[0] = CMD_BYE;
	prdat->wrbuf[1] = clspec->clientno >> 8;
	prdat->wrbuf[2] = clspec->clientno & 0xff;
	Connection_write(clspec->tcpconn, prdat->wrbuf, 3, 0);
	unregisterConnection(clspec->tcpconn, client);
    }
}

int Protocol_init(const Config *config)
{
    if (cfg) return -1;
    cfg = config;
    reconnWait = 0;
    reconnTicking = 0;
    if (!config->sockClient)
    {
	sockserver = Server_createUnix(config, CCM_WAIT, 5);
	if (!sockserver) return -1;
	Event_register(Server_clientConnected(sockserver), 0,
		sockClientConnected, 0);
	Event_register(Server_clientDisconnected(sockserver), 0,
		sockClientDisconnected, 0);
    }
    if (config->remotehost)
    {
	pendingtcp = Connection_createTcpClient(config, 0);
	if (!pendingtcp)
	{
	    Server_destroy(sockserver);
	    sockserver = 0;
	    return -1;
	}
	Event_register(Connection_connected(pendingtcp), 0,
		tcpConnectionEstablished, 0);
	Event_register(Connection_closed(pendingtcp), 0,
		tcpConnectionLost, 0);
	Event_register(Service_eventsDone(), 0, disableReconnTick, 0);
    }
    else
    {
	tcpserver = Server_createTcp(config, CCM_NORMAL, 0);
	if (!tcpserver)
	{
	    Server_destroy(sockserver);
	    sockserver = 0;
	    return -1;
	}
	Event_register(Server_clientConnected(tcpserver), 0,
		tcpClientConnected, 0);
    }
    Service_setTickInterval(5000);
    return 0;
}

int Protocol_done(void)
{
    if (!cfg) return -1;
    if (tcpclient) Connection_close(tcpclient);
    if (pendingtcp) Connection_close(pendingtcp);
    Server_destroy(sockserver);
    Server_destroy(tcpserver);
    tcpclient = 0;
    sockserver = 0;
    tcpserver = 0;
    cfg = 0;
    return 0;
}

