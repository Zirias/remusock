#include "config.h"

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifndef PIDFILE
#define PIDFILE "/var/run/remusockd.pid"
#endif

static void usage(char *prgname)
{
    fprintf(stderr, "Usage: %s [-bcfpr] [address] [pidfile] [remotehost]\n"
	    "\t\tsocket port\n",
	    prgname);
    fputs("\n\t-b address     when listening, only bind to this address "
	    "\t               instead of any\n"
	    "\t-c             open unix domain socket as client\n"
	    "\t-f             run in foreground\n"
	    "\t-p pidfile     use `pidfile' instead of compile-time default\n"
	    "\t-r remotehost  connect to `remotehost' instead of listening\n"
	    "\n"
	    "\t   socket      unix domain socket to open\n"
	    "\t   port        TCP port to connect to or listen on\n\n",
	    stderr);
}

int Config_fromOpts(Config *config, int argc, char **argv)
{
    int endflags = 0;
    int escapedash = 0;
    int needaddress = 0;
    int needpidfile = 0;
    int needremotehost = 0;
    int needsocket = 1;
    int needport = 1;
    int arg;

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
	    if (needaddress || needpidfile || needremotehost)
	    {
		usage(prgname);
		return -1;
	    }

	    for (++o; *o; ++o)
	    {
		switch (*o)
		{
		    case 'b':
			needaddress = 1;
			while (needaddress == needpidfile
				|| needaddress == needremotehost)
			{
			    ++needaddress;
			}
			break;

		    case 'c':
			config->sockClient = 1;
			break;

		    case 'f':
			config->daemonize = 0;
			break;

		    case 'p':
			needpidfile = 1;
			while (needpidfile == needaddress
				|| needpidfile == needremotehost)
			{
			    ++needpidfile;
			}
			break;

		    case 'r':
			needremotehost = 1;
			while (needremotehost == needaddress
				|| needremotehost == needpidfile)
			{
			    ++needremotehost;
			}
			break;

		    default:
			if (needaddress > needpidfile
				&& needaddress > needremotehost)
			{
			    config->bindaddr = o;
			    needaddress = 0;
			    goto next;
			}
			else if (needpidfile > needaddress
				&& needpidfile > needremotehost)
			{
			    config->pidfile = o;
			    needpidfile = 0;
			    goto next;
			}
			else if (needremotehost > needaddress
				&& needremotehost > needpidfile)
			{
			    config->remotehost = o;
			    needremotehost = 0;
			    goto next;
			}
			else
			{
			    usage(prgname);
			    return -1;
			}
		}
	    }
	}
	else
	{
	    if (needaddress > needpidfile
		    && needaddress > needremotehost)
	    {
		config->bindaddr = o;
		needaddress = 0;
	    }
	    else if (needpidfile > needaddress
		    && needpidfile > needremotehost)
	    {
		config->pidfile = o;
		needpidfile = 0;
	    }
	    else if (needremotehost > needaddress
		    && needremotehost > needpidfile)
	    {
		config->remotehost = o;
		needremotehost = 0;
	    }
	    else if (needsocket)
	    {
		config->sockname = o;
		needsocket = 0;
	    }
	    else if (needport)
	    {
		char *endp;
		errno = 0;
		long portno = strtol(o, &endp, 10);
		if (errno == ERANGE || *endp || portno < 0 || portno > 65535)
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
	    endflags = 1;
	}
next:	;
    }
    if (needaddress || needpidfile || needremotehost || needsocket || needport)
    {
	usage(prgname);
	return -1;
    }
    return 0;
}

