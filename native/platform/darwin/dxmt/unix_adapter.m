// Darwin symbols expected by the pinned DXMT unixlib. No Wine runtime is loaded in the agent.
#include "../guest/protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

static struct raven_control *control;

void raven_unixlib_init(struct raven_control *shared)
{
	__atomic_store_n(&control, shared, __ATOMIC_RELEASE);
}

// DXMT uses this from native Metal completion callbacks, which cannot enter a PE API on a Darwin worker thread.
int32_t NtSetEvent(void *event, void *previous)
{
	struct raven_control *shared = __atomic_load_n(&control, __ATOMIC_ACQUIRE);
	if (!shared || !event || previous)
		return (int32_t)0xc000000dU;
	for (unsigned i = 0; i < RAVEN_UNIX_EVENT_SLOTS; ++i) {
		uint64_t empty = 0;
		if (__atomic_compare_exchange_n(&shared->unix_events[i], &empty, (uint64_t)event, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
			return 0;
	}
	fprintf(stderr, "raven DXMT: native event completion queue is full\n");
	return (int32_t)0xc000009aU;
}

static int32_t create_listener(void *args)
{
	uint64_t *result = args;
	*result = (uint64_t)[[MTLSharedEventListener alloc] initWithDispatchQueue:dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0)];
	return *result ? 0 : (int32_t)0xc0000017U;
}

static int32_t start_listener(void *args)
{
	// The listener's dispatch queue is already running. Never enter a permanent CFRunLoop on the one real guest thread.
	(void)args;
	return 0;
}

static int32_t destroy_listener(void *args)
{
	[(MTLSharedEventListener *)*(uint64_t *)args release];
	return 0;
}

static int32_t notify_event(void *args)
{
	// Layout of v0.80's unixcall_mtlsharedevent_setevent: four 64-bit handles/value.
	const uint64_t *params = args;
	id<MTLSharedEvent> event = (id<MTLSharedEvent>)params[0];
	void *handle = (void *)params[1];
	MTLSharedEventListener *listener = (MTLSharedEventListener *)params[2];
	[event notifyListener:listener atValue:params[3] block:^(id<MTLSharedEvent> completed, uint64_t value) {
		(void)completed;
		(void)value;
		NtSetEvent(handle, NULL);
	}];
	return 0;
}

void *raven_unixlib_wrap_table(void *original, uint32_t count)
{
	if (count != 132)
		return NULL;
	void **table = malloc(count * sizeof(*table));
	if (!table)
		return NULL;
	memcpy(table, original, count * sizeof(*table));
	table[104] = notify_event;
	table[108] = create_listener;
	table[109] = start_listener;
	table[110] = destroy_listener;
	return table;
}
