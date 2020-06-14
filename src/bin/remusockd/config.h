#ifndef REMUSOCKD_CONFIG_H
#define REMUSOCKD_CONFIG_H

typedef struct
{
    const char *pidfile;
    const char *sockname;
    const char *remotehost;
    int sockClient;
    int daemonize;
    int port;
} Config;

int Config_fromOpts(Config *config, int argc, char **argv);

#endif
