#ifndef REMUSOCKD_CLIENT_H
#define REMUSOCKD_CLIENT_H

#include <stdint.h>

typedef struct Config Config;
typedef struct Connection Connection;

Connection *Connection_createTcpClient(const Config *config,
	uint8_t readOffset);
Connection *Connection_createUnixClient(const Config *config,
	uint8_t readOffset);

#endif
