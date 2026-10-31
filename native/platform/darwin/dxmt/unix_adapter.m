// Darwin symbols expected by the pinned DXMT unixlib. No Wine runtime is loaded in the agent.
#include "../guest/protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

static struct raven_control *control;
extern void raven_dxmt_wrap_presentation(void **table);

// Logical Windows threads can overlap scopes on the default scheduler's pthread. Parallel pthreads must own independent pools.
// Keep native temporaries until every overlapping scope on this pthread ends.
static __thread NSAutoreleasePool *shared_pool;
static __thread unsigned pool_scopes;

@interface RavenDXMTPool : NSObject
@end
@implementation RavenDXMTPool
- (void)dealloc
{
	if (--pool_scopes == 0) {
		NSAutoreleasePool *pool = shared_pool;
		shared_pool = nil;
		[pool drain];
	}
	[super dealloc];
}
@end

static int32_t create_pool(void *args)
{
	if (pool_scopes++ == 0)
		shared_pool = [[NSAutoreleasePool alloc] init];
	*(uint64_t *)args = (uint64_t)[[RavenDXMTPool alloc] init];
	return 0;
}

struct raven_control *raven_dxmt_control(void)
{
	return __atomic_load_n(&control, __ATOMIC_ACQUIRE);
}

uint32_t raven_unixlib_protocol_version(void)
{
	return RAVEN_GUEST_VERSION;
}

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

#if defined(__aarch64__)
#include <pthread.h>
struct native_buffer {
	uint64_t object, address, size, references;
	struct native_buffer *next;
};
static struct native_buffer *buffers;
static pthread_mutex_t buffers_lock = PTHREAD_MUTEX_INITIALIZER;
static int32_t (*original_retain)(void *), (*original_release)(void *);
static int32_t (*original_buffer)(void *), (*original_heap_buffer)(void *);
static void register_buffer(uint64_t object, uint64_t *info)
{
	if (!object || !info[2] || !info[0]) return;
	struct native_buffer *record = malloc(sizeof(*record));
	if (!record) abort();
	*record = (struct native_buffer){object, info[2], info[0], 1, NULL};
	pthread_mutex_lock(&buffers_lock);
	record->next = buffers;
	buffers = record;
	pthread_mutex_unlock(&buffers_lock);
}
static int32_t new_buffer(void *args)
{
	int32_t status = original_buffer(args);
	uint64_t *p = args;
	if (!status) register_buffer(p[2], (uint64_t *)p[1]);
	return status;
}
static int32_t new_heap_buffer(void *args)
{
	int32_t status = original_heap_buffer(args);
	uint64_t *p = args;
	if (!status) register_buffer(p[3], (uint64_t *)p[1]);
	return status;
}
static int32_t retain_buffer(void *args)
{
	pthread_mutex_lock(&buffers_lock);
	for (struct native_buffer *p = buffers; p; p = p->next)
		if (p->object == *(uint64_t *)args) { ++p->references; break; }
	int32_t status = original_retain(args);
	pthread_mutex_unlock(&buffers_lock);
	return status;
}
static int32_t release_buffer(void *args)
{
	pthread_mutex_lock(&buffers_lock);
	for (struct native_buffer **p = &buffers; *p; p = &(*p)->next)
		if ((*p)->object == *(uint64_t *)args) {
			if (!--(*p)->references) { struct native_buffer *old = *p; *p = old->next; free(old); }
			break;
		}
	int32_t status = original_release(args);
	pthread_mutex_unlock(&buffers_lock);
	return status;
}
// Copies only a live buffer's exported CPU range, holding its release lock.
int raven_dxmt_read_memory(uint64_t address, void *destination, size_t size)
{
	int found = 0;
	pthread_mutex_lock(&buffers_lock);
	for (struct native_buffer *p = buffers; p; p = p->next)
		if (address >= p->address && address - p->address <= p->size && size <= p->size - (address - p->address)) {
			memcpy(destination, (void *)address, size);
			found = 1;
			break;
		}
	pthread_mutex_unlock(&buffers_lock);
	return found;
}
#endif

void *raven_unixlib_wrap_table(void *original, uint32_t count)
{
	// Both pinned tables share the first 132 entries; the source pin appends 14.
	if (count != 132 && count != 146)
		return NULL;
	void **table = malloc(count * sizeof(*table));
	if (!table)
		return NULL;
	memcpy(table, original, count * sizeof(*table));
	table[10] = create_pool;
	table[104] = notify_event;
	table[108] = create_listener;
	table[109] = start_listener;
	table[110] = destroy_listener;
#if defined(__aarch64__)
	original_retain = table[0]; original_release = table[1]; original_buffer = table[18];
	table[0] = retain_buffer; table[1] = release_buffer; table[18] = new_buffer;
	if (count == 146) { original_heap_buffer = table[141]; table[141] = new_heap_buffer; }
#endif
	raven_dxmt_wrap_presentation(table);
	return table;
}
