#ifndef REMUSOCKD_TCPSERVER_H
#define REMUSOCKD_TCPSERVER_H

typedef struct TcpServer TcpServer;

TcpServer *TcpServer_create(int port);
void TcpServer_destroy(TcpServer *self);

#endif
