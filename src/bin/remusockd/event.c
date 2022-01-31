#include "event.h"
#include "util.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

#define EVCHUNKSIZE 4

typedef struct EvHandler
{
    void *receiver;
    EventHandler handler;
    int id;
} EvHandler;

struct Event
{
    void *sender;
    EvHandler *handlers;
    size_t size;
    size_t capa;
    int dirty;
};

Event *Event_create(void *sender)
{
    Event *self = xmalloc(sizeof *self);
    self->sender = sender;
    self->handlers = 0;
    self->size = 0;
    self->capa = 0;
    self->dirty = 0;
    return self;
}

void Event_register(Event *self, void *receiver, EventHandler handler, int id)
{
    if (self->dirty)
    {
	for (size_t pos = 0; pos < self->size; ++pos)
	{
	    if (!self->handlers[pos].handler)
	    {
		--self->size;
		if (pos < self->size)
		{
		    memmove(self->handlers + pos, self->handlers + pos + 1,
			    (self->size - pos) * sizeof *self->handlers);
		}
		--pos;
	    }
	}
	self->dirty = 0;
    }
    if (self->size == self->capa)
    {
        self->capa += EVCHUNKSIZE;
        self->handlers = xrealloc(self->handlers,
                self->capa * sizeof *self->handlers);
    }
    self->handlers[self->size].receiver = receiver;
    self->handlers[self->size].handler = handler;
    self->handlers[self->size].id = id;
    ++self->size;
}

void Event_unregister(
	Event *self, void *receiver, EventHandler handler, int id)
{
    size_t pos;
    for (pos = 0; pos < self->size; ++pos)
    {
        if (self->handlers[pos].receiver == receiver
                && self->handlers[pos].handler == handler
		&& self->handlers[pos].id == id)
        {
	    self->handlers[pos].handler = 0;
	    self->dirty = 1;
            break;
        }
    }
}

void Event_raise(Event *self, int id, void *args)
{
    for (size_t i = 0; i < self->size; ++i)
    {
	if (self->handlers[i].id == id && self->handlers[i].handler)
	{
	    if (!args && id) args = &id;
	    self->handlers[i].handler(self->handlers[i].receiver,
		    self->sender, args);
	}
    }
}

void Event_destroy(Event *self)
{
    if (!self) return;
    free(self->handlers);
    free(self);
}

