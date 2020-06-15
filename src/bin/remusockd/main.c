#include "config.h"
#include "daemon.h"
#include "log.h"
#include "service.h"
#include "syslog.h"
#include "tcpserver.h"

#include <stdlib.h>
#include <unistd.h>

static int dmain(void *data)
{
    Config *config = data;
    TcpServer *tcpserver = 0;

    Service_init(config);

    if (config->daemonize)
    {
	setSyslogLogger(LOG_DAEMON, 0);
    }

    logfmt(L_INFO, "socket: %s", config->sockname);
    if (config->remotehost)
    {
	logfmt(L_INFO, "remote: %s:%d", config->remotehost, config->port);
    }
    else
    {
	logfmt(L_INFO, "listening on %d", config->port);
	tcpserver = TcpServer_create(config);
	if (!tcpserver)
	{
	    Service_done();
	    return EXIT_FAILURE;
	}
    }

    int rc = Service_run();
    TcpServer_destroy(tcpserver);
    Service_done();
    return rc;
}

int main(int argc, char **argv)
{
    Config config;
    if (Config_fromOpts(&config, argc, argv) < 0) return EXIT_FAILURE;

    if (config.daemonize)
    {
	setSyslogLogger(LOG_DAEMON, 1);
	return daemon_run(dmain, &config, config.pidfile);
    }
    else
    {
	setFileLogger(stderr);
	return dmain(&config);
    }
}

