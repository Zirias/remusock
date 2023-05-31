#include "config.h"
#include "remusock.h"
#include "tcpclient.h"
#include "tcpserver.h"

#include <poser/core/client.h>
#include <poser/core/server.h>

static TcpServer *server;
static TcpClient *client;

int RemUSock_init(const Config *config)
{
    if (server || client) return -1;

    PSC_UnixClientOpts *sockopts = 0;
    PSC_Server *sockserver = 0;

    if (config->sockClient)
    {
	sockopts = PSC_UnixClientOpts_create(config->sockname);
    }
    else
    {
	PSC_UnixServerOpts *opts = PSC_UnixServerOpts_create(config->sockname);
	PSC_UnixServerOpts_owner(opts, config->sockuid, config->sockgid);
	PSC_UnixServerOpts_mode(opts, config->sockmode);
	sockserver = PSC_Server_createUnix(opts);
	PSC_UnixServerOpts_destroy(opts);
	if (!sockserver) return -1;
	PSC_Server_disable(sockserver);
    }

    if (config->remotehost)
    {
	PSC_TcpClientOpts *opts = PSC_TcpClientOpts_create(
		config->remotehost, config->port);
	if (config->numericHosts) PSC_TcpClientOpts_numericHosts(opts);
	client = TcpClient_create(opts, sockserver, sockopts);
    }
    else
    {
	PSC_TcpServerOpts *opts = PSC_TcpServerOpts_create(config->port);
	for (int i = 0; i < MAXBINDS && config->bindaddr[i]; ++i)
	{
	    PSC_TcpServerOpts_bind(opts, config->bindaddr[i]);
	}
	if (config->numericHosts) PSC_TcpServerOpts_numericHosts(opts);
	server = TcpServer_create(opts, sockserver, sockopts);
    }

    if (!server && !client)
    {
	PSC_UnixClientOpts_destroy(sockopts);
	PSC_Server_destroy(sockserver);
	return -1;
    }

    return 0;
}

void RemUSock_done(void)
{
    TcpClient_destroy(client);
    TcpServer_destroy(server);
    client = 0;
    server = 0;
}

