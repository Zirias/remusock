#include "config.h"
#include "protocol.h"

#include <poser/core.h>
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
#define CONNRMBUFSZ 1024

static const Config *cfg;

static PSC_Server *sockserver;
static PSC_Server *tcpserver;
static PSC_Connection *tcpclient;
static PSC_Connection *pendingtcp;
static PSC_TcpClientOpts *tcpopts;
static int reconnWait;
static int reconnTicking;
static char connrmbuf[CONNRMBUFSZ];

typedef struct ClientSpec
{
    PSC_Connection *tcpconn;
    PSC_Connection *sockconn;
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
    uint8_t wrbuf[PRDBUFSZ];
    uint8_t rdbuf[PRDBUFSZ];
} TcpProtoData;

static const char *connRemote(const PSC_Connection *conn)
{
    const char *remAddr = PSC_Connection_remoteAddr(conn);
    const char *remHost = PSC_Connection_remoteHost(conn);
    if (remAddr && remHost)
    {
	snprintf(connrmbuf, CONNRMBUFSZ, "%s [%s]", remHost, remAddr);
	return connrmbuf;
    }
    if (remHost) return remHost;
    if (remAddr) return remAddr;
    return "<unknown>";
}

static TcpProtoData *TcpProtoData_create(void)
{
    TcpProtoData *self = PSC_malloc(sizeof *self);
    self->clients = PSC_malloc(LISTCHUNK * sizeof *self->clients);
    self->state = TPS_IDENT;
    self->size = 0;
    self->capa = LISTCHUNK;
    self->rdexpect = 0;
    self->idleTicks = 0;
    self->nwriteconns = 0;
    self->rdbufpos = 0;
    return self;
}

static uint16_t registerConnectionAt(PSC_Connection *tcpconn,
	PSC_Connection *sockconn, uint16_t pos)
{
    TcpProtoData *prdat = PSC_Connection_data(tcpconn);
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
	prdat->clients = PSC_realloc(prdat->clients,
		prdat->capa * sizeof *prdat->clients);
	memset(prdat->clients + prdat->size, 0, prdat->capa - prdat->size);
    }
    ClientSpec *clspec = PSC_malloc(sizeof *clspec);
    clspec->tcpconn = tcpconn;
    clspec->sockconn = sockconn;
    clspec->clientno = pos;
    prdat->clients[pos] = clspec;
    PSC_Connection_setData(sockconn, clspec, 0);
    if (pos >= prdat->size) prdat->size = pos+1;
    return pos;
}

static uint16_t registerConnection(PSC_Connection *tcpconn,
	PSC_Connection *sockconn)
{
    TcpProtoData *prdat = PSC_Connection_data(tcpconn);
    uint16_t pos;
    for (pos = 0; pos < prdat->size; ++pos)
    {
	if (!prdat->clients[pos]) break;
    }
    return registerConnectionAt(tcpconn, sockconn, pos);
}

static ClientSpec *connectionAt(PSC_Connection *tcpconn, uint16_t pos)
{
    TcpProtoData *prdat = PSC_Connection_data(tcpconn);
    if (pos >= prdat->size) return 0;
    return prdat->clients[pos];
}

static void unregisterConnection(PSC_Connection *tcpconn,
	PSC_Connection *sockconn)
{
    TcpProtoData *prdat = PSC_Connection_data(tcpconn);
    ClientSpec *clspec = PSC_Connection_data(sockconn);
    if (prdat && clspec)
    {
	PSC_Connection_setData(sockconn, 0, 0);
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
	    PSC_Connection *sockconn = self->clients[pos]->sockconn;
	    unregisterConnection(self->clients[pos]->tcpconn, sockconn);
	    PSC_Connection_close(sockconn, 0);
	}
    }
    free(self->clients);
    free(self);
}

static void sockDataSent(void *receiver, void *sender, void *args)
{
    (void)sender;
    (void)receiver;

    PSC_Connection *tcpconn = args;
    TcpProtoData *prdat = PSC_Connection_data(tcpconn);
    if (!--prdat->nwriteconns)
    {
	PSC_Connection_confirmDataReceived(tcpconn);
    }
}

static void tcpDataSent(void *receiver, void *sender, void *args)
{
    (void)sender;
    (void)receiver;

    PSC_Connection_confirmDataReceived(args);
}

static void tcpTick(void *receiver, void *sender, void *args)
{
    (void)sender;
    (void)args;

    PSC_Connection *tcpconn = receiver;
    TcpProtoData *prdat = PSC_Connection_data(tcpconn);
    uint8_t ticks = ++prdat->idleTicks;
    if (prdat->state == TPS_IDENT && ticks == IDENTTICKS)
    {
	PSC_Log_fmt(PSC_L_INFO, "protocol: timeout waiting for ident from %s",
		connRemote(tcpconn));
	PSC_Connection_close(tcpconn, 0);
    }
    else if (ticks == CLOSETICKS)
    {
	PSC_Log_fmt(PSC_L_INFO,
		"protocol: closing unresponsive connection to %s",
		connRemote(tcpconn));
	PSC_Connection_close(tcpconn, 0);
    }
    else if (ticks == PINGTICKS)
    {
	PSC_Log_fmt(PSC_L_DEBUG, "protocol: pinging idle connection to %s",
		connRemote(tcpconn));
	prdat->wrbuf[0] = CMD_PING;
	PSC_Connection_sendAsync(tcpconn, prdat->wrbuf, 1, 0);
    }
}

static void sendConnStateCmd(PSC_Connection *tcpconn, uint8_t cmd,
	uint16_t clientno)
{
    PSC_Log_fmt(PSC_L_DEBUG, "protocol: sending %c %04x to %s", cmd,
	    clientno, connRemote(tcpconn));
    TcpProtoData *prdat = PSC_Connection_data(tcpconn);
    prdat->wrbuf[0] = cmd;
    prdat->wrbuf[1] = clientno >> 8;
    prdat->wrbuf[2] = clientno & 0xff;
    PSC_Connection_sendAsync(tcpconn, prdat->wrbuf, 3, 0);
}

static void sockConnectionLost(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)args;

    PSC_Connection *sock = sender;
    ClientSpec *client = PSC_Connection_data(sock);
    PSC_Log_fmt(PSC_L_INFO, "protocol: lost socket connection to %s",
	    connRemote(sock));
    if (client && client->tcpconn)
    {
	sendConnStateCmd(client->tcpconn, CMD_BYE, client->clientno);
	unregisterConnection(client->tcpconn, sock);
    }
}

static void sockConnectionEstablished(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)args;

    PSC_Connection *sockconn = sender;
    ClientSpec *clspec = PSC_Connection_data(sockconn);
    if (clspec && clspec->tcpconn)
    {
	sendConnStateCmd(clspec->tcpconn, CMD_CONNECT, clspec->clientno);
    }
}

static void sockDataReceived(void *receiver, void *sender, void *args)
{
    (void)receiver;

    PSC_Connection *sockconn = sender;
    PSC_EADataReceived *dra = args;
    ClientSpec *clspec = PSC_Connection_data(sockconn);
    if (clspec && clspec->tcpconn)
    {
	TcpProtoData *prdat = PSC_Connection_data(clspec->tcpconn);
	PSC_EADataReceived_markHandling(dra);
	uint16_t sz = PSC_EADataReceived_size(dra);
	PSC_Log_fmt(PSC_L_DEBUG, "protocol: sending %c %04x %04x to %s",
		CMD_DATA, clspec->clientno, sz, connRemote(clspec->tcpconn));
	prdat->wrbuf[0] = CMD_DATA;
	prdat->wrbuf[1] = clspec->clientno >> 8;
	prdat->wrbuf[2] = clspec->clientno & 0xff;
	prdat->wrbuf[3] = sz >> 8;
	prdat->wrbuf[4] = sz & 0xff;
	PSC_Connection_sendAsync(clspec->tcpconn, prdat->wrbuf, 5, 0);
	PSC_Connection_sendAsync(clspec->tcpconn, PSC_EADataReceived_buf(dra),
		sz, sockconn);
    }
}

static void busySent(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)sender;
    (void)args;

    PSC_Connection *conn = sender;
    PSC_Connection_close(conn, 0);
}

static void tcpDataReceived(void *receiver, void *sender, void *args)
{
    (void)receiver;

    PSC_Connection *tcpconn = sender;
    PSC_EADataReceived *dra = args;
    TcpProtoData *prdat = PSC_Connection_data(tcpconn);
    const uint8_t *buf = PSC_EADataReceived_buf(dra);
    uint16_t sz = PSC_EADataReceived_size(dra);
    PSC_Log_fmt(PSC_L_DEBUG,
	    "protocol: received TCP data from %s in local state %d",
	    connRemote(tcpconn), prdat->state);
    prdat->idleTicks = 0;
    uint16_t dpos = 0;
    while (dpos < sz)
    {
	switch (prdat->state)
	{
	    case TPS_IDENT:
		if (buf[dpos] != CMD_IDENT || ++dpos == sz) goto error;
		if (buf[dpos] != ARG_SERVER
			&& buf[dpos] != ARG_CLIENT) goto error;
		if (sockserver && buf[dpos] == ARG_SERVER)
		{
		    PSC_Log_fmt(PSC_L_INFO, "protocol: "
			    "dropping connection to other socket server at %s",
			    connRemote(tcpconn));
		    goto error;
		}
		if (!sockserver && buf[dpos] == ARG_CLIENT)
		{
		    PSC_Log_fmt(PSC_L_INFO, "protocol: "
			    "dropping connection to other socket client at %s",
			    connRemote(tcpconn));
		    goto error;
		}
		++dpos;
		prdat->state = TPS_DEFAULT;
		if (!tcpserver)
		{
		    prdat->wrbuf[0] = CMD_IDENT;
		    prdat->wrbuf[1] = sockserver ? ARG_SERVER : ARG_CLIENT;
		    PSC_Connection_sendAsync(tcpconn, prdat->wrbuf, 2, 0);
		}
		break;

	    case TPS_DEFAULT:
		switch (buf[dpos])
		{
		    case CMD_PING:
			prdat->wrbuf[0] = CMD_PONG;
			PSC_Connection_sendAsync(tcpconn, prdat->wrbuf, 1, 0);
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
		prdat->rdbuf[prdat->rdbufpos++] = buf[dpos++];
		prdat->state = TPS_RDCMD;
		break;

	    case TPS_RDCMD:
		prdat->rdbuf[prdat->rdbufpos++] = buf[dpos++];
		if (!--prdat->rdexpect)
		{
		    uint16_t clientno =
			(prdat->rdbuf[1] << 8) | prdat->rdbuf[2];
		    PSC_Connection *sockclient;
		    ClientSpec *client;
		    switch (prdat->rdbuf[0])
		    {
			case CMD_HELLO:
			    PSC_Log_fmt(PSC_L_DEBUG,
				    "protocol: received %c %04x from %s",
				    CMD_HELLO, clientno, connRemote(tcpconn));
			    sockclient = PSC_Connection_createUnixClient(
				    cfg->sockname);
			    if (sockclient &&
				    registerConnectionAt(tcpconn,
					sockclient, clientno) == clientno)
			    {
				PSC_Event_register(
					PSC_Connection_connected(sockclient),
					0, sockConnectionEstablished, 0);
				PSC_Event_register(
					PSC_Connection_dataReceived(
					    sockclient),
					0, sockDataReceived, 0);
				PSC_Event_register(
					PSC_Connection_dataSent(sockclient),
					0, sockDataSent, 0);
				PSC_Event_register(
					PSC_Connection_closed(sockclient),
					0, sockConnectionLost, 0);
				PSC_Log_fmt(PSC_L_INFO,
					"protocol: new remote socket "
					"client from %s on %s",
					connRemote(tcpconn),
					connRemote(sockclient));
			    }
			    else
			    {
				PSC_Log_fmt(PSC_L_WARNING,
					"protocol: cannot connect "
					"new remote socket client from %s",
					connRemote(tcpconn));
				if (sockclient) PSC_Connection_close(
					sockclient, 0);
				sendConnStateCmd(tcpconn, CMD_BYE, clientno);
			    }
			    prdat->state = TPS_DEFAULT;
			    break;
			case CMD_CONNECT:
			    PSC_Log_fmt(PSC_L_DEBUG,
				    "protocol: received %c %04x from %s",
				    CMD_CONNECT, clientno,
				    connRemote(tcpconn));
			    client = connectionAt(tcpconn, clientno);
			    if (!client)
			    {
				PSC_Log_fmt(PSC_L_INFO,
					"protocol: ignored accepted "
					"socket client from %s "
					"(already closed)",
					connRemote(tcpconn));
				break;
			    }
			    sockclient = client->sockconn;
			    PSC_Log_fmt(PSC_L_INFO,
				    "protocol: remote socket client "
				    "accepted from %s",
				    connRemote(tcpconn));
			    PSC_Connection_activate(sockclient);
			    prdat->state = TPS_DEFAULT;
			    break;
			case CMD_BYE:
			    PSC_Log_fmt(PSC_L_DEBUG,
				    "protocol: received %c %04x from %s",
				    CMD_BYE, clientno, connRemote(tcpconn));
			    client = connectionAt(tcpconn, clientno);
			    if (!client)
			    {
				PSC_Log_fmt(PSC_L_INFO,
					"protocol: ignored closed "
					"socket client from %s "
					"(already closed)",
					connRemote(tcpconn));
				break;
			    }
			    sockclient = client->sockconn;
			    PSC_Log_fmt(PSC_L_INFO,
				    "protocol: remote socket client "
				    "from %s disconnected on %s",
				    connRemote(tcpconn),
				    connRemote(sockclient));
			    unregisterConnection(tcpconn, sockclient);
			    PSC_Connection_close(sockclient, 0);
			    prdat->state = TPS_DEFAULT;
			    break;
			case CMD_DATA:
			    prdat->clientno = clientno;
			    prdat->rdexpect =
				(prdat->rdbuf[3] << 8) | prdat->rdbuf[4];
			    PSC_Log_fmt(PSC_L_DEBUG,
				    "protocol: received %c %04x %04x from %s",
				    CMD_DATA, clientno, prdat->rdexpect,
				    connRemote(tcpconn));
			    prdat->state = TPS_RDDATA;
			    break;
		    }
		    prdat->rdbufpos = 0;
		}
		break;

	    case TPS_RDDATA:
		{
		    uint16_t chunksz = sz - dpos;
		    if (chunksz > prdat->rdexpect) chunksz = prdat->rdexpect;
		    ClientSpec *client = connectionAt(
			    tcpconn, prdat->clientno);
		    if (client)
		    {
			++prdat->nwriteconns;
			PSC_EADataReceived_markHandling(dra);
			PSC_Connection_sendAsync(client->sockconn, buf + dpos,
				chunksz, tcpconn);
		    }
		    dpos += chunksz;
		    prdat->rdexpect -= chunksz;
		    if (!prdat->rdexpect)
		    {
			prdat->state = TPS_DEFAULT;
			if (!client)
			{
			    PSC_Log_fmt(PSC_L_INFO,
				    "protocol: ignored data from %s "
				    "for closed socket",
				    connRemote(tcpconn));
			}
		    }
		}
		break;
	}
    }
    return;

error:
    PSC_Log_fmt(PSC_L_INFO, "protocol: protocol error from %s",
	    connRemote(tcpconn));
    PSC_Connection_close(tcpconn, 0);
}

static void tcpReconnect(void *receiver, void *sender, void *args);

static void disableReconnTick(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)sender;
    (void)args;

    if (reconnTicking && pendingtcp)
    {
	PSC_Event_unregister(PSC_Service_tick(), 0, tcpReconnect, 0);
	reconnTicking = 0;
    }
}

static void tcpConnectionLost(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)args;

    PSC_Connection *conn = sender;
    if (conn != pendingtcp)
    {
	PSC_Event_unregister(PSC_Service_tick(), conn, tcpTick, 0);
	PSC_Log_fmt(PSC_L_INFO, "protocol: lost TCP connection to %s",
		connRemote(conn));
    }
    if (conn == tcpclient || conn == pendingtcp)
    {
	if (conn == tcpclient)
	{
	    tcpclient = 0;
	}
	if (!tcpserver)
	{
	    PSC_Log_msg(PSC_L_DEBUG, "protocol: scheduling TCP reconnect");
	    PSC_Event_register(PSC_Service_tick(), 0, tcpReconnect, 0);
	    reconnTicking = 1;
	    reconnWait = pendingtcp ? RECONNTICKS : 1;
	    pendingtcp = 0;
	}
    }
}

static void tcpConnectionEstablished(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)args;

    if (tcpclient || sender != pendingtcp)
    {
	PSC_Service_panic("protocol: unexpected TCP connection");
    }
    tcpclient = sender;
    pendingtcp = 0;
    PSC_Event_register(PSC_Connection_dataReceived(tcpclient), 0,
	    tcpDataReceived, 0);
    PSC_Event_register(PSC_Connection_dataSent(tcpclient), 0, tcpDataSent, 0);
    PSC_Event_register(PSC_Service_tick(), tcpclient, tcpTick, 0);
    PSC_Connection_setData(tcpclient, TcpProtoData_create(),
	    TcpProtoData_delete);
    PSC_Log_fmt(PSC_L_INFO, "protocol: TCP connection established to %s",
	    connRemote(tcpclient));
}

static void tcpReconnect(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)sender;
    (void)args;

    if (!--reconnWait)
    {
	PSC_Log_msg(PSC_L_DEBUG, "protocol: attempting to reconnect TCP");
	pendingtcp = PSC_Connection_createTcpClient(tcpopts);
	if (!pendingtcp)
	{
	    reconnWait = RECONNTICKS;
	    return;
	}
	PSC_Event_register(PSC_Connection_connected(pendingtcp), 0,
		tcpConnectionEstablished, 0);
	PSC_Event_register(PSC_Connection_closed(pendingtcp), 0,
		tcpConnectionLost, 0);
    }
}

static void tcpClientConnected(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)sender;

    PSC_Connection *client = args;
    if (sockserver)
    {
	if (tcpclient)
	{
	    PSC_Log_fmt(PSC_L_DEBUG,
		    "protocol: rejecting second TCP connection from "
		    "%s to socket server", connRemote(client));
	    PSC_Event_register(PSC_Connection_dataSent(client), 0,
		    busySent, 0);
	    PSC_Connection_sendAsync(client,
		    (const uint8_t *)"busy.\n", 6, client);
	    return;
	}
	tcpclient = client;
    }
    TcpProtoData *prdat = TcpProtoData_create();
    PSC_Connection_setData(client, prdat, TcpProtoData_delete);
    PSC_Event_register(PSC_Connection_closed(client), 0, tcpConnectionLost, 0);
    PSC_Event_register(PSC_Connection_dataReceived(client), 0,
	    tcpDataReceived, 0);
    PSC_Event_register(PSC_Connection_dataSent(client), 0, tcpDataSent, 0);
    PSC_Log_fmt(PSC_L_INFO, "protocol: TCP client connected from %s",
	    connRemote(client));
    PSC_Event_register(PSC_Service_tick(), client, tcpTick, 0);
    prdat->wrbuf[0] = CMD_IDENT;
    prdat->wrbuf[1] = sockserver ? ARG_SERVER : ARG_CLIENT;
    PSC_Connection_sendAsync(client, prdat->wrbuf, 2, 0);
}

static void sockClientConnected(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)sender;

    PSC_Connection *client = args;
    PSC_Log_fmt(PSC_L_INFO, "protocol: new socket client on %s",
	    connRemote(client));
    if (!tcpclient)
    {
	PSC_Connection_close(client, 0);
	return;
    }
    PSC_Event_register(PSC_Connection_dataReceived(client), 0,
	    sockDataReceived, 0);
    PSC_Event_register(PSC_Connection_dataSent(client), 0, sockDataSent, 0);
    uint16_t clientno = registerConnection(tcpclient, client);
    sendConnStateCmd(tcpclient, CMD_HELLO, clientno);
}

static void sockClientDisconnected(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)sender;

    PSC_Connection *client = args;
    ClientSpec *clspec = PSC_Connection_data(client);
    PSC_Log_fmt(PSC_L_INFO, "protocol: socket client disconnected on %s",
	    connRemote(client));
    if (clspec && clspec->tcpconn)
    {
	sendConnStateCmd(clspec->tcpconn, CMD_BYE, clspec->clientno);
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
	PSC_UnixServerOpts *opts = PSC_UnixServerOpts_create(config->sockname);
	PSC_UnixServerOpts_owner(opts, config->sockuid, config->sockgid);
	PSC_UnixServerOpts_mode(opts, config->sockmode);
	PSC_UnixServerOpts_connWait(opts);
	sockserver = PSC_Server_createUnix(opts);
	PSC_UnixServerOpts_destroy(opts);
	if (!sockserver) return -1;
	PSC_Event_register(PSC_Server_clientConnected(sockserver), 0,
		sockClientConnected, 0);
	PSC_Event_register(PSC_Server_clientDisconnected(sockserver), 0,
		sockClientDisconnected, 0);
    }
    if (config->remotehost)
    {
	tcpopts = PSC_TcpClientOpts_create(config->remotehost, config->port);
	if (config->numericHosts) PSC_TcpClientOpts_numericHosts(tcpopts);
	pendingtcp = PSC_Connection_createTcpClient(tcpopts);
	if (!pendingtcp)
	{
	    PSC_Server_destroy(sockserver);
	    sockserver = 0;
	    return -1;
	}
	PSC_Event_register(PSC_Connection_connected(pendingtcp), 0,
		tcpConnectionEstablished, 0);
	PSC_Event_register(PSC_Connection_closed(pendingtcp), 0,
		tcpConnectionLost, 0);
	PSC_Event_register(PSC_Service_eventsDone(), 0, disableReconnTick, 0);
    }
    else
    {
	PSC_TcpServerOpts *opts = PSC_TcpServerOpts_create(config->port);
	for (int i = 0; i < MAXBINDS && config->bindaddr[i]; ++i)
	{
	    PSC_TcpServerOpts_bind(opts, config->bindaddr[i]);
	}
	if (config->numericHosts) PSC_TcpServerOpts_numericHosts(opts);
	tcpserver = PSC_Server_createTcp(opts);
	PSC_TcpServerOpts_destroy(opts);
	if (!tcpserver)
	{
	    PSC_Server_destroy(sockserver);
	    sockserver = 0;
	    return -1;
	}
	PSC_Event_register(PSC_Server_clientConnected(tcpserver), 0,
		tcpClientConnected, 0);
    }
    return 0;
}

int Protocol_done(void)
{
    if (!cfg) return -1;
    if (tcpclient) PSC_Connection_close(tcpclient, 0);
    if (pendingtcp) PSC_Connection_close(pendingtcp, 0);
    PSC_Server_destroy(sockserver);
    PSC_Server_destroy(tcpserver);
    PSC_TcpClientOpts_destroy(tcpopts);
    tcpclient = 0;
    sockserver = 0;
    tcpserver = 0;
    tcpopts = 0;
    cfg = 0;
    return 0;
}

