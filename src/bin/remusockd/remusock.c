#include "config.h"
#include "remusock.h"
#include "tcpclient.h"
#include "tcpserver.h"

#include <poser/core.h>

static TcpServer *server;
static TcpClient *client;
static PSC_HashTable *hashes;

static int checkhash(void *receiver, const PSC_CertInfo *cert)
{
    (void)receiver;

    if (hashes)
    {
	const char *hash = PSC_CertInfo_fingerprintStr(cert);
	if (!PSC_HashTable_get(hashes, hash)) return 0;
    }

    PSC_Log_fmt(PSC_L_INFO, "Accepted client cert %s",
	    PSC_CertInfo_subject(cert));
    return 1;
}

int RemUSock_init(const Config *config)
{
    if (server || client) return -1;

    PSC_UnixClientOpts *sockopts = 0;
    PSC_Server *sockserver = 0;

    if (config->hashes)
    {
	hashes = PSC_HashTable_create(6);
	PSC_List *hashlist = PSC_List_fromString(config->hashes, ":");
	PSC_ListIterator *i = PSC_List_iterator(hashlist);
	while (PSC_ListIterator_moveNext(i))
	{
	    PSC_HashTable_set(hashes, PSC_ListIterator_current(i),
		    (void *)config, 0);
	}
	PSC_ListIterator_destroy(i);
	PSC_List_destroy(hashlist);
    }

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
	if (config->tls)
	{
	    PSC_TcpClientOpts_enableTls(opts, config->cert, config->key);
	    if (config->noverify) PSC_TcpClientOpts_disableCertVerify(opts);
	}
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
	if (config->tls)
	{
	    PSC_TcpServerOpts_enableTls(opts, config->cert, config->key);
	    if (config->cacerts || config->hashes)
	    {
		PSC_TcpServerOpts_requireClientCert(opts, config->cacerts);
		PSC_TcpServerOpts_validateClientCert(opts, 0, checkhash);
	    }
	}
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
    PSC_HashTable_destroy(hashes);
    client = 0;
    server = 0;
    hashes = 0;
}

