#include "config.h"
#include "log.h"

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define ARGBUFSZ 16

#ifndef PIDFILE
#define PIDFILE "/var/run/remusockd.pid"
#endif

static void usage(char *prgname)
{
    fprintf(stderr, "Usage: %s [-cfv]\n"
	    "\t\t[-b address] [-p pidfile] [-r remotehost] socket port\n",
	    prgname);
    fputs("\n\t-b address     when listening, only bind to this address\n"
	    "\t               instead of any\n"
	    "\t-c             open unix domain socket as client\n"
	    "\t-f             run in foreground\n"
	    "\t-p pidfile     use `pidfile' instead of compile-time default\n"
	    "\t-r remotehost  connect to `remotehost' instead of listening\n"
	    "\t-v             verbose logging output\n"
	    "\n"
	    "\tsocket         unix domain socket to open\n"
	    "\tport           TCP port to connect to or listen on\n\n",
	    stderr);
}

static int addArg(char *args, int *idx, char opt)
{
    if (*idx == ARGBUFSZ) return -1;
    memmove(args+1, args, (*idx)++);
    args[0] = opt;
    return 0;
}

static int optArg(Config *config, char *args, int *idx, char *op)
{
    if (!*idx) return -1;
    switch (args[--*idx])
    {
	case 'b':
	    config->bindaddr = op;
	    break;
	case 'p':
	    config->pidfile = op;
	    break;
	case 'r':
	    config->remotehost = op;
	    break;
	default:
	    return -1;
    }
    return 0;
}

int Config_fromOpts(Config *config, int argc, char **argv)
{
    int endflags = 0;
    int escapedash = 0;
    int needsocket = 1;
    int needport = 1;
    int arg;
    int naidx = 0;
    char needargs[ARGBUFSZ];

    memset(config, 0, sizeof *config);
    config->pidfile = PIDFILE;
    config->daemonize = 1;

    char *prgname = "remusockd";
    if (argc > 0) prgname = argv[0];

    for (arg = 1; arg < argc; ++arg)
    {
	char *o = argv[arg];
	if (!escapedash && *o == '-' && o[1] == '-' && !o[2])
	{
	    escapedash = 1;
	    continue;
	}

	if (!endflags && !escapedash && *o == '-' && o[1])
	{
	    if (naidx)
	    {
		usage(prgname);
		return -1;
	    }

	    for (++o; *o; ++o)
	    {
		switch (*o)
		{
		    case 'b':
		    case 'p':
		    case 'r':
			if (addArg(needargs, &naidx, *o) < 0) return -1;
			break;

		    case 'c':
			config->sockClient = 1;
			break;

		    case 'f':
			config->daemonize = 0;
			break;

		    case 'v':
			setMaxLogLevel(L_DEBUG);
			break;

		    default:
			if (optArg(config, needargs, &naidx, o) < 0)
			{
			    usage(prgname);
			    return -1;
			}
			goto next;
		}
	    }
	}
	else
	{
	    if (optArg(config, needargs, &naidx, o) < 0)
	    {
		if (needsocket)
		{
		    config->sockname = o;
		    needsocket = 0;
		}
		else if (needport)
		{
		    char *endp;
		    errno = 0;
		    long portno = strtol(o, &endp, 10);
		    if (errno == ERANGE || *endp
			    || portno < 0 || portno > 65535)
		    {
			usage(prgname);
			return -1;
		    }
		    config->port = portno;
		    needport = 0;
		}
		else
		{
		    usage(prgname);
		    return -1;
		}
	    }
	    endflags = 1;
	}
next:	;
    }
    if (naidx || needsocket || needport)
    {
	usage(prgname);
	return -1;
    }
    return 0;
}

