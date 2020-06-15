#ifndef REMUSOCKD_EVENT_H
#define REMUSOCKD_EVENT_H

typedef void (*EventHandler)(void *receiver, int id,
	const void *sender, const void *args);

typedef struct Event Event;

Event *Event_create(const void *sender);
void Event_register(
	Event *self, void *receiver, EventHandler handler, int id);
void Event_unregister(
	Event *self, void *receiver, EventHandler handler, int id);
void Event_raise(Event *self, int id, const void *args);
void Event_destroy(Event *self);

#endif
