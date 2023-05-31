#include "protocol.h"
#include "tcpclient.h"

#include <poser/core.h>
#include <stdlib.h>

#define RECONNTICKSNORM	6
#define RECONNTICKSERR 30

struct TcpClient
{
    PSC_Connection *tcpclient;
    PSC_TcpClientOpts *clientopts;
    PSC_Server *sockserver;
    PSC_UnixClientOpts *sockopts;
    int ticks;
};

static void deleteproto(void *proto);
static void identsent(void *receiver, void *sender, void *args);
static void identcheck(void *receiver, void *sender, void *args);
static void identtimeout(void *receiver, void *sender, void *args);
static void checkreconn(void *receiver, void *sender, void *args);
static void connlost(void *receiver, void *sender, void *args);
static void connected(void *receiver, void *sender, void *args);
static void connectioncreated(void *receiver, PSC_Connection *client);
static void connect(TcpClient *self);

static void deleteproto(void *proto)
{
    Protocol_destroy(proto);
}

static void identsent(void *receiver, void *sender, void *args)
{
    (void)args;

    TcpClient *self = receiver;
    PSC_Connection *client = sender;

    PSC_Event_unregister(PSC_Connection_dataSent(client), self, identsent, 0);
    PSC_Connection_confirmDataReceived(client);

    Protocol *proto = Protocol_create(client,
	    self->sockserver, self->sockopts);
    PSC_Connection_setData(client, proto, deleteproto);
}

static void identcheck(void *receiver, void *sender, void *args)
{
    TcpClient *self = receiver;
    PSC_Connection *client = sender;
    PSC_EADataReceived *dra = args;

    PSC_Event_unregister(PSC_Service_tick(), self, identtimeout, 0);
    PSC_Event_unregister(PSC_Connection_dataReceived(client), self,
	    identcheck, 0);

    const uint8_t *buf = PSC_EADataReceived_buf(dra);
    if (buf[0] != CMD_IDENT) goto protoerr;

    switch (buf[1])
    {
	case ARG_SERVER:
	    if (self->sockserver)
	    {
		PSC_Log_msg(PSC_L_WARNING, "TcpClient: server identified as "
			"socket server, expected socket client");
		goto err;
	    }
	    break;

	case ARG_CLIENT:
	    if (!self->sockserver)
	    {
		PSC_Log_msg(PSC_L_WARNING, "TcpClient: server identified as "
			"socked client, expected socket server");
		goto err;
	    }
	    break;

	default:
	    goto protoerr;
    }

    PSC_EADataReceived_markHandling(dra);
    PSC_Event_register(PSC_Connection_dataSent(client), self, identsent, 0);
    PSC_Connection_sendAsync(client,
	    self->sockserver ? idsrv : idcli, 2, self);
    return;

protoerr:
    PSC_Log_msg(PSC_L_WARNING,
	    "TcpClient: received unexpected data from server");
err:
    PSC_Connection_close(client, 0);
}

static void identtimeout(void *receiver, void *sender, void *args)
{
    (void)sender;
    (void)args;

    TcpClient *self = receiver;

    if (!--self->ticks)
    {
	PSC_Connection_close(self->tcpclient, 0);
	PSC_Event_unregister(PSC_Service_tick(), self, identtimeout, 0);
    }
}

static void checkreconn(void *receiver, void *sender, void *args)
{
    (void)sender;
    (void)args;

    TcpClient *self = receiver;
    if (!--self->ticks)
    {
	PSC_Event_unregister(PSC_Service_tick(), self, checkreconn, 0);
	connect(self);
    }
}

static void connlost(void *receiver, void *sender, void *args)
{
    (void)sender;

    TcpClient *self = receiver;

    self->tcpclient = 0;

    if (args)
    {
	PSC_Log_msg(PSC_L_INFO,
		"TcpClient: connection lost, scheduling reconnection");
	self->ticks = RECONNTICKSNORM;
	PSC_Event_unregister(PSC_Service_tick(), self, identtimeout, 0);
    }
    else
    {
	PSC_Log_msg(PSC_L_INFO,
		"TcpClient: failed to connect, scheduling reconnection");
	self->ticks = RECONNTICKSERR;
    }

    PSC_Event_register(PSC_Service_tick(), self, checkreconn, 0);
}

static void connected(void *receiver, void *sender, void *args)
{
    (void)args;

    TcpClient *self = receiver;
    PSC_Connection *client = sender;

    PSC_Event_unregister(PSC_Connection_connected(client), self, connected, 0);

    PSC_Event_register(PSC_Connection_dataReceived(client), self,
	    identcheck, 0);
    PSC_Event_register(PSC_Service_tick(), self, identtimeout, 0);

    self->ticks = IDENTTICKS;
    PSC_Connection_receiveBinary(client, 2);
}

static void connectioncreated(void *receiver, PSC_Connection *client)
{
    TcpClient *self = receiver;

    if (!client)
    {
	PSC_Log_msg(PSC_L_INFO,
		"TcpClient: failed to connect, scheduling reconnection");
	self->ticks = RECONNTICKSERR;
	PSC_Event_register(PSC_Service_tick(), self, checkreconn, 0);
	return;
    }

    self->tcpclient = client;
    PSC_Event_register(PSC_Connection_connected(client), self, connected, 0);
    PSC_Event_register(PSC_Connection_closed(client), self, connlost, 0);
}

static void connect(TcpClient *self)
{
    if (PSC_Connection_createTcpClientAsync(self->clientopts, self,
		connectioncreated) < 0)
    {
	PSC_Service_panic("TcpClient: failed to request client creation.");
    }
}

TcpClient *TcpClient_create(PSC_TcpClientOpts *opts, PSC_Server *sockserver,
	PSC_UnixClientOpts *sockopts)
{
    TcpClient *self = PSC_malloc(sizeof *self);
    self->tcpclient = 0;
    self->clientopts = opts;
    self->sockserver = sockserver;
    self->sockopts = sockopts;
    self->ticks = 0;
    connect(self);
    return self;
}

void TcpClient_destroy(TcpClient *self)
{
    if (!self) return;
    if (self->tcpclient)
    {
	PSC_Event_unregister(PSC_Connection_closed(self->tcpclient),
		self, connlost, 0);
	PSC_Connection_close(self->tcpclient, 0);
    }
    PSC_Server_destroy(self->sockserver);
    PSC_UnixClientOpts_destroy(self->sockopts);
    PSC_TcpClientOpts_destroy(self->clientopts);
    free(self);
}

