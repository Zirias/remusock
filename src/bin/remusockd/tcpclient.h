#ifndef REMUSOCKD_TCPCLIENT_H
#define REMUSOCKD_TCPCLIENT_H

typedef struct TcpClient TcpClient;

typedef struct PSC_Server PSC_Server;
typedef struct PSC_UnixClientOpts PSC_UnixClientOpts;
typedef struct PSC_TcpClientOpts PSC_TcpClientOpts;

TcpClient *TcpClient_create(PSC_TcpClientOpts *opts, PSC_Server *sockserver,
	PSC_UnixClientOpts *sockopts);
void TcpClient_destroy(TcpClient *self);

#endif
