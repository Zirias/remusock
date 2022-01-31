#ifndef REMUSOCKD_SERVER_H
#define REMUSOCKD_SERVER_H

#include "connopts.h"

#include <stdint.h>

typedef struct Config Config;
typedef struct Event Event;
typedef struct Server Server;

Server *Server_createTcp(const Config *config, ConnectionCreateMode mode,
	uint8_t readOffset);
Server *Server_createUnix(const Config *config, ConnectionCreateMode mode,
	uint8_t readOffset);
Event *Server_clientConnected(Server *self);
Event *Server_clientDisconnected(Server *self);
void Server_destroy(Server *self);

#endif
