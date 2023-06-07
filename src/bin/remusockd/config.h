#ifndef REMUSOCKD_CONFIG_H
#define REMUSOCKD_CONFIG_H

#ifndef MAXBINDS
#define MAXBINDS 4
#endif

typedef struct Config
{
    char **argv;
    const char *bindaddr[MAXBINDS];
    const char *pidfile;
    const char *sockname;
    const char *remotehost;
    const char *cert;
    const char *key;
    const char *cacerts;
    const char *hashes;
    long sockuid;
    long sockgid;
    int sockClient;
    int daemonize;
    int port;
    int numericHosts;
    int sockmode;
    int tls;
    int noverify;
} Config;

int Config_fromOpts(Config *config, int argc, char **argv);

#endif
