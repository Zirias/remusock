#include "protocol.h"
#include "tcpserver.h"

#include <poser/core.h>
#include <stdlib.h>

struct TcpServer
{
    PSC_Server *tcpserver;
    PSC_Server *sockserver;
    PSC_UnixClientOpts *sockopts;
};

typedef struct ClientRec
{
    TcpServer *server;
    PSC_Connection *client;
    int identticks;
} ClientRec;

static void identtimeout(void *receiver, void *sender, void *args);
static void identabort(void *receiver, void *sender, void *args);
static void deleteproto(void *proto);
static void identcheck(void *receiver, void *sender, void *args);
static void identsent(void *receiver, void *sender, void *args);
static void clientConnected(void *receiver, void *sender, void *args);
static void clientDisconnected(void *receiver, void *sender, void *args);

static void identtimeout(void *receiver, void *sender, void *args)
{
    (void)sender;
    (void)args;

    ClientRec *cr = receiver;

    if (!--cr->identticks)
    {
	PSC_Connection_close(cr->client, 0);
    }
}

static void identabort(void *receiver, void *sender, void *args)
{
    (void)sender;
    (void)args;

    ClientRec *cr = receiver;

    PSC_Event_unregister(PSC_Connection_dataSent(cr->client), cr,
	    identsent, 0);
    PSC_Event_unregister(PSC_Service_tick(), cr, identtimeout, 0);
}

static void deleteproto(void *proto)
{
    Protocol_destroy(proto);
}

static void identcheck(void *receiver, void *sender, void *args)
{
    ClientRec *cr = receiver;
    PSC_Connection *client = sender;
    PSC_EADataReceived *dra = args;

    PSC_Event_unregister(PSC_Service_tick(), cr, identtimeout, 0);
    PSC_Event_unregister(PSC_Connection_dataReceived(client), cr,
	    identcheck, 0);
    PSC_Event_unregister(PSC_Connection_closed(client), cr, identabort, 0);

    const uint8_t *buf = PSC_EADataReceived_buf(dra);
    if (buf[0] != CMD_IDENT) goto protoerr;

    switch (buf[1])
    {
	case ARG_SERVER:
	    if (cr->server->sockserver)
	    {
		PSC_Log_fmt(PSC_L_WARNING, "TcpServer: client from %s "
			"identified as socket server, expected socket client",
			PSC_Connection_remoteAddr(client));
		goto err;
	    }
	    break;

	case ARG_CLIENT:
	    if (!cr->server->sockserver)
	    {
		PSC_Log_fmt(PSC_L_WARNING, "TcpServer: client from %s "
			"identified as socket client, expected socket server",
			PSC_Connection_remoteAddr(client));
		goto err;
	    }
	    break;

	default:
	    goto protoerr;
    }

    Protocol *proto = Protocol_create(client,
	    cr->server->sockserver, cr->server->sockopts);
    PSC_Connection_setData(client, proto, deleteproto);
    return;

protoerr:
    PSC_Log_fmt(PSC_L_WARNING, "TcpServer: received unexpected data from %s",
	    PSC_Connection_remoteAddr(client));
err:
    PSC_Connection_close(client, 0);
}

static void identsent(void *receiver, void *sender, void *args)
{
    (void)args;

    ClientRec *cr = receiver;
    PSC_Connection *client = sender;

    PSC_Event_register(PSC_Service_tick(), cr, identtimeout, 0);
    PSC_Event_register(PSC_Connection_dataReceived(client), cr, identcheck, 0);
    PSC_Event_unregister(PSC_Connection_dataSent(client), cr, identsent, 0);

    PSC_Connection_receiveBinary(client, 2);
    PSC_Connection_resume(client);
}

static void clientConnected(void *receiver, void *sender, void *args)
{
    TcpServer *self = receiver;
    PSC_Server *server = sender;
    PSC_Connection *client = args;

    const uint8_t *idmsg;
    if (self->sockserver)
    {
	PSC_Server_disable(server);
	idmsg = idsrv;
    }
    else
    {
	idmsg = idcli;
    }

    ClientRec *cr = PSC_malloc(sizeof *cr);
    cr->server = self;
    cr->client = client;
    cr->identticks = IDENTTICKS;
    PSC_Connection_setData(client, cr, free);

    PSC_Event_register(PSC_Connection_closed(client), cr, identabort, 0);
    PSC_Event_register(PSC_Connection_dataSent(client), cr, identsent, 0);

    PSC_Connection_sendAsync(client, idmsg, 2, cr);
    PSC_Connection_pause(client);
}

static void clientDisconnected(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)args;

    PSC_Server_enable(sender);
}

TcpServer *TcpServer_create(PSC_TcpServerOpts *opts, PSC_Server *sockserver,
	PSC_UnixClientOpts *sockopts)
{
    PSC_Server *tcpserver = PSC_Server_createTcp(opts);
    PSC_TcpServerOpts_destroy(opts);

    if (!tcpserver) return 0;

    TcpServer *self = PSC_malloc(sizeof *self);
    self->tcpserver = tcpserver;
    self->sockserver = sockserver;
    self->sockopts = sockopts;

    PSC_Event_register(PSC_Server_clientConnected(tcpserver),
	    self, clientConnected, 0);

    if (sockserver)
    {
	PSC_Event_register(PSC_Server_clientDisconnected(tcpserver),
		self, clientDisconnected, 0);
    }

    return self;
}

void TcpServer_destroy(TcpServer *self)
{
    if (!self) return;
    if (self->sockserver)
    {
	PSC_Event_unregister(PSC_Server_clientDisconnected(self->tcpserver),
		self, clientDisconnected, 0);
    }
    PSC_Event_unregister(PSC_Server_clientConnected(self->tcpserver),
	    self, clientConnected, 0);
    PSC_Server_destroy(self->tcpserver);
    PSC_Server_destroy(self->sockserver);
    PSC_UnixClientOpts_destroy(self->sockopts);
    free(self);
}

