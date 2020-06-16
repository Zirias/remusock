#ifndef REMUSOCKD_TCPSERVER_H
#define REMUSOCKD_TCPSERVER_H

#include <stdint.h>

typedef struct Config Config;
typedef struct Event Event;
typedef struct Server Server;

Server *Server_create(int sockfd, char *path);
Server *Server_createTcp(const Config *config);
Server *Server_createUnix(const Config *config);
Event *Server_clientConnected(Server *self);
Event *Server_clientDisconnected(Server *self);
Event *Server_dataReceived(Server *self);
Event *Server_dataSent(Server *self);
int Server_writeBuffer(Server *self, int fd, char **buf, uint16_t *sz);
int Server_commitWrite(Server *self, int fd, uint16_t sz);
void Server_destroy(Server *self);

#endif
