#ifndef REMUSOCKD_CLIENT_H
#define REMUSOCKD_CLIENT_H

typedef struct Config Config;
typedef struct Client Client;

Client *Client_create(int sockfd);
Client *Client_createTcp(const Config *config);
Client *Client_createUnix(const Config *config);
void Client_destroy(Client *self);

#endif
