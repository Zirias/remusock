#define _DEFAULT_SOURCE

#include "daemon.h"
#include "log.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int outfd;

int daemon_run(const daemon_main dmain, void *data,
	const char *pidfile, int waitLaunched)
{
    pid_t pid, sid;
    int rc = EXIT_FAILURE;
    FILE *pf = 0;
    
    if (pidfile)
    {
	pf = fopen(pidfile, "r");
	if (pf)
	{
	    int prc = fscanf(pf, "%d", &pid);
	    fclose(pf);
	    pf = 0;
	    if (prc < 1 || kill(pid, 0) < 0)
	    {
		logfmt(L_WARNING, "removing stale pidfile `%s'", pidfile);
		if (unlink(pidfile) < 0)
		{
		    logfmt(L_ERROR, "cannot remove `%s'", pidfile);
		    goto done;
		}
	    }
	    else
	    {
		logmsg(L_ERROR, "daemon already running");
		goto done;
	    }
	}
	pf = fopen(pidfile, "w");
	if (!pf)
	{
	    logfmt(L_ERROR, "cannot open pidfile `%s' for writing", pidfile);
	    goto done;
	}
    }

    int pfd[2];
    if (!waitLaunched || pipe(pfd) < 0)
    {
	pfd[0] = -1;
	pfd[1] = -1;
    }
    outfd = -1;

    pid = fork();

    if (pid < 0)
    {
	if (pfd[0] >= 0) close(pfd[0]);
	if (pfd[1] >= 0) close(pfd[1]);
	logmsg(L_ERROR, "failed to fork (1)");
	goto done;
    }

    if (pid > 0)
    {
	if (pfd[1] >= 0) close(pfd[1]);
	if (pfd[0] >= 0)
	{
	    char buf[256];
	    ssize_t sz;
	    while ((sz = read(pfd[0], buf, sizeof buf)) > 0)
	    {
		write(STDERR_FILENO, buf, sz);
	    }
	    close(pfd[0]);
	}
	return EXIT_SUCCESS;
    }
    if (pfd[0] >= 0) close(pfd[0]);

    sid = setsid();
    if (sid < 0)
    {
	logmsg(L_ERROR, "setsid() failed");
	goto done;
    }

    struct sigaction handler;
    memset(&handler, 0, sizeof handler);
    handler.sa_handler = SIG_IGN;
    sigemptyset(&handler.sa_mask);
    sigaction(SIGTERM, &handler, 0);
    sigaction(SIGINT, &handler, 0);
    sigaction(SIGHUP, &handler, 0);
    sigaction(SIGUSR1, &handler, 0);
#ifndef DEBUG
    sigaction(SIGSTOP, &handler, 0);
#endif

    pid = fork();

    if (pid < 0)
    {
	logmsg(L_ERROR, "failed to fork (2)");
	goto done;
    }

    if (pid > 0)
    {
	if (pf)
	{
	    fprintf(pf, "%d\n", pid);
	    fclose(pf);
	}
	return EXIT_SUCCESS;
    }

    if (pf)
    {
	fclose(pf);
	pf = 0;
    }

    if (chdir("/") < 0)
    {
	logmsg(L_ERROR, "chdir(\"/\") failed");
	goto done;
    }

    umask(0);
    close(STDIN_FILENO);

    outfd = open("/dev/null", O_WRONLY);
    if (pfd[1] >= 0)
    {
	if (outfd < 0)
	{
	    close(STDOUT_FILENO);
	}
	else
	{
	    dup2(outfd, STDOUT_FILENO);
	}
	dup2(pfd[1], STDERR_FILENO);
    }
    else if (outfd < 0)
    {
	close(STDOUT_FILENO);
	close(STDERR_FILENO);
    }
    else
    {
	dup2(outfd, STDOUT_FILENO);
	dup2(outfd, STDERR_FILENO);
    }
    if (pfd[1] >= 0)
    {
	close(pfd[1]);
    }
    else if (outfd >= 0)
    {
	close(outfd);
	outfd = -1;
    }

    logmsg(L_INFO, "forked into background");
    rc = dmain(data);
    if (pidfile) unlink(pidfile);

done:
    if (pf) fclose(pf);
    return rc;
}

void daemon_launched(void)
{
    if (outfd >= 0)
    {
	dup2(outfd, STDERR_FILENO);
	close(outfd);
	outfd = -1;
    }
}

