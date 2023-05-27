#include "config.h"
#include "protocol.h"

#include <poser/core.h>
#include <stdlib.h>

#define LOGIDENT "remusockd"

static void startup(void *receiver, void *sender, void *args)
{
    (void)sender;

    Config *config = receiver;
    if (Protocol_init(config) < 0)
    {
	PSC_EAStartup_return(args, EXIT_FAILURE);
    }
    else
    {
	char *cmdline = PSC_joinstr(" ", config->argv);
	PSC_Log_fmt(PSC_L_INFO, "starting with commandline: %s", cmdline);
	free(cmdline);
    }
}

static void shutdown(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)sender;
    (void)args;

    Protocol_done();
}

int main(int argc, char **argv)
{
    Config config;
    if (Config_fromOpts(&config, argc, argv) < 0) return EXIT_FAILURE;

    PSC_RunOpts_init(config.pidfile);
    PSC_RunOpts_runas(config.sockuid, config.sockgid);
    PSC_RunOpts_enableDefaultLogging(LOGIDENT);
    if (!config.daemonize) PSC_RunOpts_foreground();

    PSC_Event_register(PSC_Service_prestartup(), &config, startup, 0);
    PSC_Event_register(PSC_Service_shutdown(), 0, shutdown, 0);

    return PSC_Service_run();
}

