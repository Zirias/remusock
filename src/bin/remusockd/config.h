#ifndef REMUSOCKD_CONFIG_H
#define REMUSOCKD_CONFIG_H

typedef struct Config
{
    char **argv;
    const char *pidfile;
    const char *sockname;
    const char *bindaddr;
    const char *remotehost;
    long sockuid;
    long sockgid;
    int sockClient;
    int daemonize;
    int port;
    int numericHosts;
    int sockmode;
} Config;

int Config_fromOpts(Config *config, int argc, char **argv);

#endif
