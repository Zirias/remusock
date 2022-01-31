#define _DEFAULT_SOURCE

#include "event.h"
#include "log.h"
#include "service.h"
#include "threadpool.h"
#include "util.h"

#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NTHREADS 8
#define QUEUESIZE 64

struct ThreadJob
{
    ThreadProc proc;
    void *arg;
    Event *finished;
    int hasCompleted;
    int timeoutTicks;
};

typedef struct Thread
{
    ThreadJob *job;
    pthread_t handle;
    pthread_mutex_t startlock;
    pthread_mutex_t donelock;
    pthread_cond_t start;
    pthread_cond_t done;
    int pipefd[2];
    int failed;
    int stoprq;
} Thread;

static Thread threads[NTHREADS];
static ThreadJob *jobQueue[QUEUESIZE];
static int queueAvail;
static int nextIdx;
static int lastIdx;
static int active;
static volatile __thread sig_atomic_t jobcanceled;

void workerInterrupt(int signum)
{
    (void) signum;
    jobcanceled = 1;
}

void *worker(void *arg)
{
    Thread *t = arg;
    t->failed = 0;
    t->stoprq = 0;
    if (pthread_mutex_lock(&t->startlock) < 0)
    {
	t->failed = 1;
	write(t->pipefd[1], "1", 1);
	return 0;
    }

    struct sigaction handler;
    memset(&handler, 0, sizeof handler);
    handler.sa_handler = workerInterrupt;
    sigemptyset(&handler.sa_mask);
    sigaddset(&handler.sa_mask, SIGUSR1);

    if (sigaction(SIGUSR1, &handler, 0) < 0)
    {
	t->failed = 1;
	write(t->pipefd[1], "1", 1);
	return 0;
    }

    if (pthread_sigmask(SIG_UNBLOCK, &handler.sa_mask, 0) < 0)
    {
	t->failed = 1;
	write(t->pipefd[1], "1", 1);
	return 0;
    }

    while (!t->stoprq)
    {
	pthread_cond_wait(&t->start, &t->startlock);
	if (t->stoprq) break;
	jobcanceled = 0;
	t->job->proc(t->job->arg);
	write(t->pipefd[1], "0", 1);
	pthread_mutex_lock(&t->donelock);
	pthread_cond_signal(&t->done);
	pthread_mutex_unlock(&t->donelock);
    }

    pthread_mutex_unlock(&t->startlock);
    return 0;
}

ThreadJob *ThreadJob_create(ThreadProc proc, void *arg, int timeoutTicks)
{
    ThreadJob *self = xmalloc(sizeof *self);
    self->proc = proc;
    self->arg = arg;
    self->finished = Event_create(self);
    self->timeoutTicks = timeoutTicks;
    self->hasCompleted = 1;
    return self;
}

Event *ThreadJob_finished(ThreadJob *self)
{
    return self->finished;
}

int ThreadJob_hasCompleted(const ThreadJob *self)
{
    return self->hasCompleted;
}

void ThreadJob_destroy(ThreadJob *self)
{
    if (!self) return;
    Event_destroy(self->finished);
    free(self);
}

int ThreadJob_canceled(void)
{
    return (int)jobcanceled;
}

static void stopThreads(int nthreads)
{
    for (int i = 0; i < nthreads; ++i)
    {
	if (pthread_kill(threads[i].handle, 0) >= 0)
	{
	    if (threads[i].job)
	    {
		threads[i].stoprq = 1;
		pthread_kill(threads[i].handle, SIGUSR1);
		pthread_cond_wait(&threads[i].done, &threads[i].donelock);
		pthread_mutex_unlock(&threads[i].donelock);
	    }
	    else
	    {
		pthread_mutex_lock(&threads[i].startlock);
		threads[i].stoprq = 1;
		pthread_cond_signal(&threads[i].start);
		pthread_mutex_unlock(&threads[i].startlock);
	    }
	}
	pthread_join(threads[i].handle, 0);
	close(threads[i].pipefd[0]);
	close(threads[i].pipefd[1]);
	pthread_cond_destroy(&threads[i].done);
	pthread_mutex_destroy(&threads[i].donelock);
	pthread_cond_destroy(&threads[i].start);
	pthread_mutex_destroy(&threads[i].startlock);
    }
}

static int enqueueJob(ThreadJob *job)
{
    if (!queueAvail) return -1;
    jobQueue[nextIdx++] = job;
    --queueAvail;
    if (nextIdx == QUEUESIZE) nextIdx = 0;
    return 0;
}

static ThreadJob *dequeueJob(void)
{
    ThreadJob *job = 0;
    while (!job)
    {
	if (queueAvail == QUEUESIZE) return 0;
	job = jobQueue[lastIdx++];
	++queueAvail;
	if (lastIdx == QUEUESIZE) lastIdx = 0;
    }
    return job;
}

static Thread *availableThread(void)
{
    for (int i = 0; i < NTHREADS; ++i)
    {
	if (!threads[i].job) return threads+i;
    }
    return 0;
}

static void startThreadJob(Thread *t, ThreadJob *j)
{
    pthread_mutex_lock(&t->startlock);
    t->job = j;
    pthread_cond_signal(&t->start);
    pthread_mutex_lock(&t->donelock);
    pthread_mutex_unlock(&t->startlock);
}

void threadJobDone(void *receiver, void *sender, void *args)
{
    (void)sender;
    (void)args;

    Thread *t = receiver;
    char buf[2];
    read(t->pipefd[0], buf, sizeof buf);
    if (t->failed)
    {
	pthread_join(t->handle, 0);
	logmsg(L_WARNING, "threadpool: restarting failed thread");
	if (pthread_create(&t->handle, 0, worker, t) < 0)
	{
	    logmsg(L_FATAL, "threadpool: error restarting thread");
	    Service_quit();
	}
	return;
    }
    pthread_cond_wait(&t->done, &t->donelock);
    Event_raise(t->job->finished, 0, t->job->arg);
    ThreadJob_destroy(t->job);
    t->job = 0;
    pthread_mutex_unlock(&t->donelock);
    ThreadJob *next = dequeueJob();
    if (next) startThreadJob(t, next);
}

void checkThreadJobs(void *receiver, void *sender, void *args)
{
    (void)receiver;
    (void)sender;
    (void)args;

    for (int i = 0; i < NTHREADS; ++i)
    {
	if (threads[i].job && threads[i].job->timeoutTicks
		&& !--threads[i].job->timeoutTicks)
	{
	    pthread_kill(threads[i].handle, SIGUSR1);
	    threads[i].job->hasCompleted = 0;
	}
    }
}

int ThreadPool_init(void)
{
    sigset_t blockmask;
    sigset_t mask;
    sigfillset(&blockmask);
    int rc = -1;
    
    if (sigprocmask(SIG_BLOCK, &blockmask, &mask) < 0)
    {
	logmsg(L_ERROR, "threadpool: cannot set signal mask");
	return rc;
    }

    memset(threads, 0, sizeof threads);

    for (int i = 0; i < NTHREADS; ++i)
    {
	if (pthread_mutex_init(&threads[i].startlock, 0) < 0)
	{
	    logmsg(L_ERROR, "threadpool: error creating mutex");
	    goto rollback;
	}
	if (pthread_cond_init(&threads[i].start, 0) < 0)
	{
	    logmsg(L_ERROR, "threadpool: error creating condition variable");
	    goto rollback_startlock;
	}
	if (pthread_mutex_init(&threads[i].donelock, 0) < 0)
	{
	    logmsg(L_ERROR, "threadpool: error creating mutex");
	    goto rollback_start;
	}
	if (pthread_cond_init(&threads[i].done, 0) < 0)
	{
	    logmsg(L_ERROR, "threadpool: error creating condition variable");
	    goto rollback_donelock;
	}
	if (pipe(threads[i].pipefd) < 0)
	{
	    logmsg(L_ERROR, "threadpool: error creating pipe");
	    goto rollback_done;
	}
	Event_register(Service_readyRead(), threads+i, threadJobDone,
		threads[i].pipefd[0]);
	if (pthread_create(&threads[i].handle, 0, worker, threads+i) < 0)
	{
	    logmsg(L_ERROR, "threadpool: error creating thread");
	    Event_unregister(Service_readyRead(), threads+i, threadJobDone,
		    threads[i].pipefd[0]);
	    goto rollback_pipe;
	}
	Service_registerRead(threads[i].pipefd[0]);
	continue;

rollback_pipe:
	close(threads[i].pipefd[0]);
	close(threads[i].pipefd[1]);
rollback_done:
	pthread_cond_destroy(&threads[i].done);
rollback_donelock:
	pthread_mutex_destroy(&threads[i].donelock);
rollback_start:
	pthread_cond_destroy(&threads[i].start);
rollback_startlock:
	pthread_mutex_destroy(&threads[i].startlock);
rollback:
	stopThreads(i);
	goto error;
    }
    rc = 0;
    Event_register(Service_tick(), 0, checkThreadJobs, 0);
    queueAvail = QUEUESIZE;
    nextIdx = 0;
    lastIdx = 0;
    active = 1;

error:
    if (sigprocmask(SIG_SETMASK, &mask, 0) < 0)
    {
	logmsg(L_ERROR, "threadpool: cannot restore signal mask");
	stopThreads(NTHREADS);
	return -1;
    }

    for (int i = 0; i < NTHREADS; ++i)
    {
	ThreadJob *next = dequeueJob();
	if (!next) break;
	startThreadJob(threads + i, next);
    }

    return rc;
}

int ThreadPool_active(void)
{
    return active;
}

int ThreadPool_enqueue(ThreadJob *job)
{
    if (active)
    {
	Thread *t = availableThread();
	if (t)
	{
	    startThreadJob(t, job);
	    return 0;
	}
    }
    return enqueueJob(job);
}

void ThreadPool_cancel(ThreadJob *job)
{
    if (active)
    {
	for (int i = 0; i < NTHREADS; ++i)
	{
	    if (threads[i].job == job)
	    {
		pthread_kill(threads[i].handle, SIGUSR1);
		threads[i].job->hasCompleted = 0;
		return;
	    }
	}
    }
    if (queueAvail != QUEUESIZE)
    {
	int i = lastIdx;
	do
	{
	    if (jobQueue[i] == job)
	    {
		job->hasCompleted = 0;
		Event_raise(job->finished, 0, job->arg);
		ThreadJob_destroy(job);
		jobQueue[i] = 0;
		return;
	    }
	    if (++i == QUEUESIZE) i = 0;
	} while ( i != nextIdx);
    }
}

void ThreadPool_done(void)
{
    stopThreads(NTHREADS);
    active = 0;
}
