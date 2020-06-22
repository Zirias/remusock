#ifndef REMUSOCKD_THREADPOOL_H
#define REMUSOCKD_THREADPOOL_H

typedef struct Event Event;
typedef struct ThreadJob ThreadJob;

typedef void (*ThreadProc)(void *arg);

ThreadJob *ThreadJob_create(ThreadProc proc, void *arg, int timeoutTicks);
Event *ThreadJob_finished(ThreadJob *self);
int ThreadJob_hasCompleted(const ThreadJob *self);

int ThreadPool_init(void);
int ThreadPool_active(void);
int ThreadPool_enqueue(ThreadJob *job);
void ThreadPool_done(void);

#endif
