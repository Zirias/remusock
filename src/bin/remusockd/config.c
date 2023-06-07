#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <poser/core/log.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#define ARGBUFSZ 16

#ifndef PIDFILE
#define PIDFILE "/var/run/remusockd.pid"
#endif

#define STR(m) XSTR(m)
#define XSTR(m) #m

static void usage(const char *prgname)
{
    fprintf(stderr, "Usage: %s [-Vcfntv] [-C CAfile] [-H hash[:hash...]]\n"
	    "\t\t[-b address] [-g group] [-m mode] [-p pidfile]\n"
	    "\t\t[-r remotehost] [-u user] socket port [cert key]\n",
	    prgname);
    fputs("\n\t-C CAfile      A file with one or more CA certificates in\n"
	    "\t               PEM format. When listening, require a client\n"
	    "\t               certificate issued by one of these CAs.\n"
	    "\t-H hash[:...]  One or more SHA-512 hashes (128 hex digits).\n"
	    "\t               When listening, require a client certificate\n"
	    "\t               matching one of these hashes (fingerprints).\n"
	    "\t-V             When connecting to a remote host with TLS,\n"
	    "\t               don't verify the server certificate\n"
	    "\t-b address     when listening, only bind to this address\n"
	    "\t               instead of any\n"
	    "\t               (can be given up to " STR(MAXBINDS) " times)\n"
	    "\t-c             open unix domain socket as client\n"
	    "\t-f             run in foreground\n"
	    "\t-g group       group name or id for the server socket,\n"
	    "\t               if a user name is given, defaults to the\n"
	    "\t               default group of that user\n"
	    "\t-m mode        permissions for the server socket in octal,\n"
	    "\t               defaults to 600\n"
	    "\t-n             numeric hosts, do not resolve remote addresses\n"
	    "\t-p pidfile     use `pidfile' instead of compile-time default\n"
	    "\t-r remotehost  connect to `remotehost' instead of listening\n"
	    "\t-t             Enable TLS. This is implied when a cert and\n"
	    "\t               key, or -C or -H are given. When listening\n"
	    "\t               with TLS enabled, cert and key are required\n"
	    "\t               and must match the hostname used to connect to\n"
	    "\t               this instance.\n"
	    "\t-u user        user name or id for the server socket\n"
	    "\t               when started as root, run as this user\n"
	    "\t-v             verbose logging output\n"
	    "\n"
	    "\tsocket         unix domain socket to open\n"
	    "\tport           TCP port to connect to or listen on\n"
	    "\tcert           Certificate to use in PEM format\n"
	    "\tkey            Private key of the cert in PEM format\n\n",
	    stderr);
}

static int addArg(char *args, int *idx, char opt)
{
    if (*idx == ARGBUFSZ) return -1;
    memmove(args+1, args, (*idx)++);
    args[0] = opt;
    return 0;
}

static int intArg(int *setting, char *op, int min, int max, int base)
{
    char *endp;
    errno = 0;
    long val = strtol(op, &endp, base);
    if (errno == ERANGE || *endp || val < min || val > max) return -1;
    *setting = val;
    return 0;
}

static int longArg(long *setting, char *op)
{
    char *endp;
    errno = 0;
    long val = strtol(op, &endp, 10);
    if (errno == ERANGE || *endp) return -1;
    *setting = val;
    return 0;
}

static int validhashes(char *str)
{
    int pos = 0;
    for (;;)
    {
	while (pos < 128)
	{
	    if (!isxdigit(*str)) return 0;
	    *str = tolower(*str);
	    ++str;
	    ++pos;
	}
	if (!*str) return 1;
	if (*str != ':') return 0;
	++str;
	pos = 0;
    }
}

static int optArg(Config *config, char *args, int *idx, char *op)
{
    if (!*idx) return -1;
    int i;
    switch (args[--*idx])
    {
	case 'C':
	    config->cacerts = op;
	    config->tls = 1;
	    break;
	case 'H':
	    if (!validhashes(op)) return -1;
	    config->hashes = op;
	    config->tls = 1;
	    break;
	case 'b':
	    for (i = 0; i < MAXBINDS; ++i)
	    {
		if (!config->bindaddr[i])
		{
		    config->bindaddr[i] = op;
		    break;
		}
	    }
	    if (i == MAXBINDS) return -1;
	    break;
	case 'g':
	    if (longArg(&config->sockgid, op) < 0)
	    {
		struct group *g;
		if (!(g = getgrnam(op))) return -1;
		config->sockgid = g->gr_gid;
	    }
	    break;
	case 'm':
	    if (intArg(&config->sockmode, op, 0, 0777, 8) < 0) return -1;
	    break;
	case 'p':
	    config->pidfile = op;
	    break;
	case 'r':
	    config->remotehost = op;
	    break;
	case 'u':
	    if (longArg(&config->sockuid, op) < 0)
	    {
		struct passwd *p;
		if (!(p = getpwnam(op))) return -1;
		config->sockuid = p->pw_uid;
		if (config->sockgid == -1)
		{
		    config->sockgid = p->pw_gid;
		}
	    }
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
    int havecert = 0;
    int needkey = 0;
    int arg;
    int naidx = 0;
    char needargs[ARGBUFSZ];
    const char onceflags[] = "CHVcfgmnprtuv";
    char seen[sizeof onceflags - 1] = {0};

    memset(config, 0, sizeof *config);
    config->pidfile = PIDFILE;
    config->daemonize = 1;
    config->sockmode = 0600;
    config->sockuid = -1;
    config->sockgid = -1;

    const char *prgname = "remusockd";
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
		const char *sip = strchr(onceflags, *o);
		if (sip)
		{
		    int si = (int)(sip - onceflags);
		    if (seen[si])
		    {
			usage(prgname);
			return -1;
		    }
		    seen[si] = 1;
		}
		switch (*o)
		{
		    case 'C':
		    case 'H':
		    case 'b':
		    case 'g':
		    case 'm':
		    case 'p':
		    case 'r':
		    case 'u':
			if (addArg(needargs, &naidx, *o) < 0) return -1;
			break;

		    case 'V':
			config->noverify = 1;
			config->tls = 1;
			break;

		    case 'c':
			config->sockClient = 1;
			break;

		    case 'f':
			config->daemonize = 0;
			break;

		    case 'n':
			config->numericHosts = 1;
			break;

		    case 't':
			config->tls = 1;
			break;

		    case 'v':
			PSC_Log_setMaxLogLevel(PSC_L_DEBUG);
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
	else if (optArg(config, needargs, &naidx, o) < 0)
	{
	    if (needsocket)
	    {
		config->sockname = o;
		needsocket = 0;
	    }
	    else if (needport)
	    {
		if (intArg(&config->port, o, 0, 65535, 10) < 0)
		{
		    usage(prgname);
		    return -1;
		}
		needport = 0;
	    }
	    else if (needkey)
	    {
		config->key = o;
		needkey = 0;
	    }
	    else if (!havecert)
	    {
		config->cert = o;
		config->tls = 1;
		havecert = 1;
		needkey = 1;
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
    if (naidx || needsocket || needport || needkey
	    || (config->remotehost && config->bindaddr[0])
	    || (config->remotehost && (config->cacerts || config->hashes))
	    || (!config->remotehost && config->tls && !config->cert)
	    || (!config->remotehost && config->noverify))
    {
	usage(prgname);
	return -1;
    }

    config->argv = argv;
    return 0;
}

