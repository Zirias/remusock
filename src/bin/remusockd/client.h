#ifndef REMUSOCKD_CLIENT_H
#define REMUSOCKD_CLIENT_H

typedef struct Config Config;
typedef struct Connection Connection;

Connection *Connection_createTcpClient(const Config *config);
Connection *Connection_createUnixClient(const Config *config);

#endif
