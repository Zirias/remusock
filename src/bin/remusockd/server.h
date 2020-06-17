#ifndef REMUSOCKD_SERVER_H
#define REMUSOCKD_SERVER_H

#include <stdint.h>

#define MAXSOCKS 4

typedef struct Config Config;
typedef struct Event Event;
typedef struct Server Server;

Server *Server_create(uint8_t nsocks, int *sockfd, char *path);
Server *Server_createTcp(const Config *config);
Server *Server_createUnix(const Config *config);
Event *Server_clientConnected(Server *self);
Event *Server_clientDisconnected(Server *self);
void Server_destroy(Server *self);

#endif
