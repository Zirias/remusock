#ifndef REMUSOCKD_TCPSERVER_H
#define REMUSOCKD_TCPSERVER_H

typedef struct Config Config;
typedef struct Event Event;
typedef struct Server Server;

Server *Server_create(int sockfd, char *path);
Server *Server_createTcp(const Config *config);
Server *Server_createUnix(const Config *config);
Event *Server_clientConnected(Server *self);
Event *Server_clientDisconnected(Server *self);
Event *Server_dataReceived(Server *self);
void Server_destroy(Server *self);

#endif
