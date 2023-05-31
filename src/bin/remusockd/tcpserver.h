#ifndef REMUSOCKD_TCPSERVER_H
#define REMUSOCKD_TCPSERVER_H

typedef struct TcpServer TcpServer;

typedef struct PSC_Server PSC_Server;
typedef struct PSC_UnixClientOpts PSC_UnixClientOpts;
typedef struct PSC_TcpServerOpts PSC_TcpServerOpts;

TcpServer *TcpServer_create(PSC_TcpServerOpts *opts, PSC_Server *sockserver,
	PSC_UnixClientOpts *sockopts);
void TcpServer_destroy(TcpServer *self);

#endif
