// Darwin symbols expected by the pinned DXMT unixlib. No Wine runtime is loaded in the agent.
#include "../guest/protocol.h"
#include "checked_memory.h"
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
// Weak entries disappear when Metal releases the object, including releases
// outside the unix table. Readback takes a strong snapshot before copying.
static NSHashTable *buffers;
static pthread_mutex_t buffers_lock = PTHREAD_MUTEX_INITIALIZER;
static int32_t (*original_buffer)(void *), (*original_heap_buffer)(void *);
static void register_buffer(uint64_t object, uint64_t *info)
{
    if (!object) return;
    info = raven_unix_checked(info, 3, sizeof(*info), 0);
    if (!info[2] || !info[0]) return;
    pthread_mutex_lock(&buffers_lock);
    if (!buffers) buffers = [[NSHashTable weakObjectsHashTable] retain];
    [buffers addObject:(id)object];
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
int raven_dxmt_read_memory(uint64_t address, void *destination, size_t size)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    pthread_mutex_lock(&buffers_lock);
    NSArray *snapshot = [[buffers allObjects] retain];
    pthread_mutex_unlock(&buffers_lock);
    int found = 0;
    for (id<MTLBuffer> buffer in snapshot) {
        uint64_t start = (uint64_t)[buffer contents];
        uint64_t length = [buffer length];
        if (start && address >= start && address - start <= length && size <= length - (address - start)) {
            memcpy(destination, (void *)address, size);
            found = 1;
            break;
        }
    }
    [snapshot release];
    [pool drain];
    return found;
}

#endif

void *raven_unixlib_wrap_table(void *original, uint32_t count)
{
	// Provisioned pins preserve these wrapper ordinals.
	const char *backend = getenv("RAVEN_D3D12_BACKEND");
	const int mullion = backend && !strcmp(backend, "mullion");
	if (mullion ? count != 197 : count != 132 && count != 146)
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
	original_buffer = table[18];
	table[18] = new_buffer;
	if (count >= 146) { original_heap_buffer = table[141]; table[141] = new_heap_buffer; }
#endif
	raven_dxmt_wrap_presentation(table);
	if (mullion) {
		extern void raven_mullion_wrap_presentation(void **);
		raven_mullion_wrap_presentation(table);
	}
	return table;
}
