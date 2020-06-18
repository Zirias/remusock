#define _DEFAULT_SOURCE

#include "service.h"
#include "config.h"
#include "event.h"
#include "log.h"

#include <grp.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/param.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

static const Config *cfg;
static Event *readyRead;
static Event *readyWrite;
static Event *shutdown;
static Event *tick;
static Event *eventsDone;

static fd_set readfds;
static fd_set writefds;
static int nread;
static int nwrite;
static int nfds;
static int running;

static sig_atomic_t shutdownRequest;
static sig_atomic_t timerTick;

static struct itimerval timer;
static struct itimerval otimer;

static void handlesig(int signum)
{
    if (signum == SIGALRM) timerTick = 1;
    else shutdownRequest = 1;
}

static void tryReduceNfds(int id)
{
    if (!nread && !nwrite)
    {
	nfds = 0;
    }
    else if (id+1 >= nfds)
    {
	int fd;
	for (fd = id; fd >= 0; --fd)
	{
	    if (FD_ISSET(fd, &readfds) || FD_ISSET(fd, &writefds))
	    {
		break;
	    }
	}
	nfds = fd+1;
    }
}

int Service_init(const Config *config)
{
    if (cfg) return -1;
    cfg = config;
    readyRead = Event_create(0);
    readyWrite = Event_create(0);
    shutdown = Event_create(0);
    tick = Event_create(0);
    eventsDone = Event_create(0);
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    nread = 0;
    nwrite = 0;
    nfds = 0;
    running = 0;
    shutdownRequest = 0;
    timerTick = 0;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = 0;
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = 0;
    return 0;
}

Event *Service_readyRead(void)
{
    return readyRead;
}

Event *Service_readyWrite(void)
{
    return readyWrite;
}

Event *Service_shutdown(void)
{
    return shutdown;
}

Event *Service_tick(void)
{
    return tick;
}

Event *Service_eventsDone(void)
{
    return eventsDone;
}

void Service_registerRead(int id)
{
    if (FD_ISSET(id, &readfds)) return;
    FD_SET(id, &readfds);
    ++nread;
    if (id >= nfds) nfds = id+1;
}

void Service_unregisterRead(int id)
{
    if (!FD_ISSET(id, &readfds)) return;
    FD_CLR(id, &readfds);
    --nread;
    tryReduceNfds(id);
}

void Service_registerWrite(int id)
{
    if (FD_ISSET(id, &writefds)) return;
    FD_SET(id, &writefds);
    ++nwrite;
    if (id >= nfds) nfds = id+1;
}

void Service_unregisterWrite(int id)
{
    if (!FD_ISSET(id, &writefds)) return;
    FD_CLR(id, &writefds);
    --nwrite;
    tryReduceNfds(id);
}

int Service_setTickInterval(unsigned msec)
{
    timer.it_interval.tv_sec = msec / 1000U;
    timer.it_interval.tv_usec = 1000U * (msec % 1000U);
    timer.it_value.tv_sec = msec / 1000U;
    timer.it_value.tv_usec = 1000U * (msec % 1000U);
    if (running) return setitimer(ITIMER_REAL, &timer, 0);
    return 0;
}

int Service_run(void)
{
    if (!cfg) return -1;

    int rc = EXIT_FAILURE;
    if (cfg->sockuid != -1 && geteuid() == 0)
    {
	if (cfg->daemonize)
	{
	    chown(cfg->pidfile, cfg->sockuid, cfg->sockgid);
	}
	if (cfg->sockgid != -1)
	{
	    gid_t gid = cfg->sockgid;
	    if (setgroups(1, &gid) < 0 || setgid(gid) < 0)
	    {
		logmsg(L_ERROR, "cannot set specified group");
		return rc;
	    }
	}
	if (setuid(cfg->sockuid) < 0)
	{
	    logmsg(L_ERROR, "cannot set specified user");
	    return rc;
	}
    }

    struct sigaction handler;
    memset(&handler, 0, sizeof handler);
    handler.sa_handler = handlesig;
    sigemptyset(&handler.sa_mask);
    sigaddset(&handler.sa_mask, SIGTERM);
    sigaddset(&handler.sa_mask, SIGINT);
    sigaddset(&handler.sa_mask, SIGALRM);
    sigset_t mask;

    if (sigprocmask(SIG_BLOCK, &handler.sa_mask, &mask) < 0)
    {
	logmsg(L_ERROR, "cannot set signal mask");
	return rc;
    }

    if (sigaction(SIGTERM, &handler, 0) < 0)
    {
	logmsg(L_ERROR, "cannot set signal handler for SIGTERM");
	goto done;
    }

    if (sigaction(SIGINT, &handler, 0) < 0)
    {
	logmsg(L_ERROR, "cannot set signal handler for SIGINT");
	goto done;
    }

    if (sigaction(SIGALRM, &handler, 0) < 0)
    {
	logmsg(L_ERROR, "cannot set signal handler for SIGALRM");
	goto done;
    }

    if (setitimer(ITIMER_REAL, &timer, &otimer) < 0)
    {
	logmsg(L_ERROR, "cannot set periodic timer");
	goto done;
    }

    rc = EXIT_SUCCESS;
    logmsg(L_INFO, "service starting");
    while (!shutdownRequest)
    {
	Event_raise(eventsDone, 0, 0);
	fd_set rfds;
	fd_set wfds;
	fd_set *r = 0;
	fd_set *w = 0;
	if (nread)
	{
	    memcpy(&rfds, &readfds, sizeof rfds);
	    r = &rfds;
	}
	if (nwrite)
	{
	    memcpy(&wfds, &writefds, sizeof wfds);
	    w = &wfds;
	}
	int src = pselect(nfds, r, w, 0, 0, &mask);
	if (shutdownRequest) break;
	if (timerTick)
	{
	    timerTick = 0;
	    Event_raise(tick, 0, 0);
	    continue;
	}
	if (src < 0)
	{
	    logmsg(L_ERROR, "pselect() failed");
	    rc = EXIT_FAILURE;
	    break;
	}
	if (w) for (int i = 0; src > 0 && i < nfds; ++i)
	{
	    if (FD_ISSET(i, w))
	    {
		--src;
		Event_raise(readyWrite, i, 0);
	    }
	}
	if (r) for (int i = 0; src > 0 && i < nfds; ++i)
	{
	    if (FD_ISSET(i, r))
	    {
		--src;
		Event_raise(readyRead, i, 0);
	    }
	}
    }

    logmsg(L_INFO, "service shutting down");
    Event_raise(shutdown, 0, 0);

done:
    if (sigprocmask(SIG_SETMASK, &mask, 0) < 0)
    {
	logmsg(L_ERROR, "cannot restore original signal mask");
	rc = EXIT_FAILURE;
    }

    if (setitimer(ITIMER_REAL, &otimer, 0) < 0)
    {
	logmsg(L_ERROR, "cannot restore original periodic timer");
	rc = EXIT_FAILURE;
    }

    return rc;
}

void Service_quit(void)
{
    shutdownRequest = 1;
}

void Service_done(void)
{
    if (!cfg) return;
    Event_destroy(eventsDone);
    Event_destroy(tick);
    Event_destroy(shutdown);
    Event_destroy(readyWrite);
    Event_destroy(readyRead);
    cfg = 0;
    shutdown = 0;
    readyWrite = 0;
    readyRead = 0;
}

