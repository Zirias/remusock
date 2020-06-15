#ifndef REMUSOCKD_TCPSERVER_H
#define REMUSOCKD_TCPSERVER_H

typedef struct Config Config;
typedef struct TcpServer TcpServer;

TcpServer *TcpServer_create(const Config *config);
void TcpServer_destroy(TcpServer *self);

#endif
