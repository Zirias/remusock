#include "config.h"
#include "daemon.h"
#include "log.h"
#include "protocol.h"
#include "service.h"
#include "syslog.h"
#include "threadpool.h"
#include "util.h"

#include <stdlib.h>
#include <unistd.h>

#define LOGIDENT "remusockd"

static int dmain(void *data)
{
    Config *config = data;

    Service_init(config);
    int rc = EXIT_FAILURE;
    if (ThreadPool_init() >= 0)
    {
	if (Protocol_init(config) >= 0)
	{
	    if (config->daemonize)
	    {
		setSyslogLogger(LOGIDENT, LOG_DAEMON, 0);
		daemon_launched();
		logsetasync(1);
	    }
	    char *cmdline = joinstr(" ", config->argv);
	    logfmt(L_INFO, "starting with commandline: %s", cmdline);
	    free(cmdline);
	    rc = Service_run();
	    Protocol_done();
	}
	ThreadPool_done();
    }
    Service_done();
    return rc;
}

int main(int argc, char **argv)
{
    Config config;
    if (Config_fromOpts(&config, argc, argv) < 0) return EXIT_FAILURE;

    if (config.daemonize)
    {
	setSyslogLogger(LOGIDENT, LOG_DAEMON, 1);
	return daemon_run(dmain, &config, config.pidfile, 1);
    }
    else
    {
	setFileLogger(stderr);
	return dmain(&config);
    }
}

