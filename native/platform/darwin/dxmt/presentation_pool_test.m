// Exercise the real x86_64 allocator without windows or captured pixels.
static void retire_published(unsigned slot);
static void retire_acknowledged(unsigned slot);
#define RAVEN_DXMT_RETIRE_PUBLISHED(slot) retire_published(slot)
#define RAVEN_DXMT_RETIRE_ACKNOWLEDGED(slot) retire_acknowledged(slot)
#define RAVEN_DXMT_RETIRE_ATTEMPTS 20
#include "presentation.m"
#include "slot_state.h"
#include <pthread.h>
#include <stdlib.h>

static struct raven_control control;
static bool acknowledge_retirement = true;
static bool pause_retirement;
static dispatch_semaphore_t paused, resume_retirement[2];
static unsigned pause_index;
struct raven_control *raven_dxmt_control(void) { return &control; }

static void check(bool ok, const char *message)
{
	if (!ok) { fprintf(stderr, "%s\n", message); exit(1); }
}

// Normal acknowledgements are synchronous, independent of worker scheduling.
static void retire_published(unsigned slot)
{
	if (acknowledge_retirement)
		__atomic_store_n(&control.surfaces[slot].state, RAVEN_SURFACE_UNUSED, __ATOMIC_RELEASE);
}

static void retire_acknowledged(unsigned slot)
{
	(void)slot;
	if (!pause_retirement) return;
	const unsigned index = __atomic_fetch_add(&pause_index, 1, __ATOMIC_RELAXED);
	check(index < 2, "bounded paused destructors");
	dispatch_semaphore_signal(paused);
	check(!dispatch_semaphore_wait(resume_retirement[index], dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), "resume paused destructor");
}

static void *destroy_drawable(void *object)
{
	@autoreleasepool { [(RavenDXMTDrawable *)object release]; }
	return NULL;
}

static unsigned send_refs(mach_port_t port)
{
	mach_port_urefs_t refs = 0;
	check(mach_port_get_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, &refs) == KERN_SUCCESS, "read send right references");
	return refs;
}

static void test_retirement(id<MTLDevice> device)
{
	struct layer_props props = {.width = 16, .height = 16, .format = 80};
	RavenDXMTDrawable *old = [[RavenDXMTDrawable alloc] initWithDevice:device window:1001 properties:&props];
	check([old acquireSlot], "reserve retirement victim");
	const unsigned slot = old->slot;
	mach_port_t port = IOSurfaceCreateMachPort(old->surface);
	control.surfaces[slot].port = port;
	check(mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, 1) == KERN_SUCCESS, "retain guard right");
	check(send_refs(port) == 2, "one transport right plus guard");
	paused = dispatch_semaphore_create(0);
	resume_retirement[0] = dispatch_semaphore_create(0);
	resume_retirement[1] = dispatch_semaphore_create(0);
	pause_retirement = true;
	pthread_t destructor;
	check(!pthread_create(&destructor, NULL, destroy_drawable, old), "start destructor");
	check(!dispatch_semaphore_wait(paused, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), "wait for UNUSED before cleanup");
	RavenDXMTDrawable *next = [[RavenDXMTDrawable alloc] initWithDevice:device window:1002 properties:&props];
	check([next acquireSlot] && next->slot == (int)slot, "acquire while destructor paused outside lock");
	check(send_refs(port) == 1, "allocator releases old right exactly once");
	mach_port_t replacement = IOSurfaceCreateMachPort(next->surface);
	control.surfaces[slot].port = replacement;
	check(mach_port_mod_refs(mach_task_self(), replacement, MACH_PORT_RIGHT_SEND, 1) == KERN_SUCCESS, "guard replacement right");
	pthread_t replacement_destructor;
	check(!pthread_create(&replacement_destructor, NULL, destroy_drawable, next), "retire replacement before old destructor resumes");
	check(!dispatch_semaphore_wait(paused, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), "pause replacement cleanup after UNUSED");
	const uint64_t replacement_ticket = retiring_slots[slot].ticket;
	dispatch_semaphore_signal(resume_retirement[0]);
	pthread_join(destructor, NULL);
	check(send_refs(port) == 1 && send_refs(replacement) == 2 && retiring_slots[slot].active &&
		retiring_slots[slot].ticket == replacement_ticket, "old ticket must not reap a newer acknowledged retirement");
	dispatch_semaphore_signal(resume_retirement[1]);
	pthread_join(replacement_destructor, NULL);
	pause_retirement = false;
	check(send_refs(replacement) == 1, "replacement destructor releases its own right exactly once");
	mach_port_deallocate(mach_task_self(), port);
	mach_port_deallocate(mach_task_self(), replacement);
	dispatch_release(paused);
	dispatch_release(resume_retirement[0]);
	dispatch_release(resume_retirement[1]);

	old = [[RavenDXMTDrawable alloc] initWithDevice:device window:1003 properties:&props];
	check([old acquireSlot], "reserve unacknowledged victim");
	const unsigned stuck = old->slot;
	port = IOSurfaceCreateMachPort(old->surface);
	control.surfaces[stuck].port = port;
	check(mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_SEND, 1) == KERN_SUCCESS, "guard quarantined right");
	acknowledge_retirement = false;
	[old release];
	check(retiring_slots[stuck].active && control.surfaces[stuck].state == RAVEN_SURFACE_RETIRE && send_refs(port) == 2,
		"timeout quarantines transport right until acknowledgement");
	next = [[RavenDXMTDrawable alloc] initWithDevice:device window:1004 properties:&props];
	check([next acquireSlot] && next->slot != (int)stuck, "quarantine leaves other slots usable");
	acknowledge_retirement = true;
	[next release];
	__atomic_store_n(&control.surfaces[stuck].state, RAVEN_SURFACE_UNUSED, __ATOMIC_RELEASE);
	next = [[RavenDXMTDrawable alloc] initWithDevice:device window:1005 properties:&props];
	check([next acquireSlot] && next->slot == (int)stuck && send_refs(port) == 1, "late ack reaps quarantine exactly once");
	[next release];
	mach_port_deallocate(mach_task_self(), port);
}

static void test_forward_right(id<MTLDevice> device)
{
	struct layer_props props = {.width = 16, .height = 16, .format = 80};
	RavenDXMTDrawable *drawable = [[RavenDXMTDrawable alloc] initWithDevice:device window:2001 properties:&props];
	mach_port_t receive;
	check(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &receive) == KERN_SUCCESS, "allocate foreign queue");
	check(mach_port_insert_right(mach_task_self(), receive, receive, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS, "queue send right");
	struct { mach_msg_header_t header; mach_msg_body_t body; mach_msg_port_descriptor_t surface; } message = {0};
	message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
	message.header.msgh_size = sizeof(message);
	message.header.msgh_remote_port = receive;
	message.body.msgh_descriptor_count = 1;
	message.surface.name = IOSurfaceCreateMachPort(drawable->surface);
	message.surface.disposition = MACH_MSG_TYPE_COPY_SEND;
	message.surface.type = MACH_MSG_PORT_DESCRIPTOR;
	check(IOSurfaceIsInUse(drawable->surface), "send right protects source before forwarding");
	check(mach_msg(&message.header, MACH_SEND_MSG, sizeof(message), 0, MACH_PORT_NULL, 0, MACH_PORT_NULL) == KERN_SUCCESS, "queue foreign surface");
	mach_port_deallocate(mach_task_self(), message.surface.name);
	check(IOSurfaceIsInUse(drawable->surface), "queued COPY_SEND protects source before receiver runs");
	struct { mach_msg_header_t header; mach_msg_body_t body; mach_msg_port_descriptor_t surface; mach_msg_max_trailer_t trailer; } received;
	check(mach_msg(&received.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(received), receive, 1000, MACH_PORT_NULL) == KERN_SUCCESS, "receive foreign surface");
	check(IOSurfaceIsInUse(drawable->surface), "received send right protects source before show");
	IOSurfaceRef imported = IOSurfaceLookupFromMachPort(received.surface.name);
	check(imported != NULL, "lookup received surface");
	IOSurfaceIncrementUseCount(imported);
	mach_port_deallocate(mach_task_self(), received.surface.name);
	check(IOSurfaceIsInUse(drawable->surface), "show use count takes over before received right is released");
	check(![drawable acquireSlot], "cannot overwrite forwarded surface across handoff");
	IOSurfaceDecrementUseCount(imported);
	CFRelease(imported);
	check(!IOSurfaceIsInUse(drawable->surface), "handoff protection ends only after display releases surface");
	mach_port_destroy(mach_task_self(), receive);
	[drawable release];
}

int main(void)
{
	@autoreleasepool {
		struct raven_surface_frame stale = {.state = RAVEN_SURFACE_DISPLAYED, .sequence = 9};
		raven_surface_release(&stale, 8);
		check(stale.state == RAVEN_SURFACE_DISPLAYED, "stale release preserves newer presentation");
		raven_surface_release(&stale, 9);
		check(stale.state == RAVEN_SURFACE_AVAILABLE, "matching release acknowledges presentation");
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (!device) return 77;
		test_forward_right(device);
		test_retirement(device);
		RavenDXMTLayer *layers[128];
		for (unsigned i = 0; i < 128; ++i) {
			layers[i] = [[RavenDXMTLayer alloc] init];
			layers[i]->window = i + 1;
			layers[i]->props.device = (uint64_t)device;
			layers[i]->props.width = layers[i]->props.height = 16;
			uint64_t args[2] = {(uint64_t)layers[i], 0};
			check(!next_drawable(args) && args[1] && layers[i]->frames.count == 1, "lazy first drawable");
			RavenDXMTDrawable *frame = (id)args[1];
			IOSurfaceIncrementUseCount(frame->surface);
			__atomic_store_n(&control.surfaces[frame->slot].state, RAVEN_SURFACE_DISPLAYED, __ATOMIC_RELEASE);
		}
		for (unsigned i = 0; i < 128; ++i) {
			uint64_t args[2] = {(uint64_t)layers[i], 0};
			check(!next_drawable(args) && layers[i]->frames.count == 2, "grow only when first frame busy");
			RavenDXMTDrawable *frame = (id)args[1];
			__atomic_store_n(&control.surfaces[frame->slot].state, RAVEN_SURFACE_AVAILABLE, __ATOMIC_RELEASE);
			control.surfaces[frame->slot].port = IOSurfaceCreateMachPort(frame->surface);
			check(control.surfaces[frame->slot].port != MACH_PORT_NULL, "create drawable send right");
			check(!next_drawable(args) && args[1] == (uint64_t)frame && layers[i]->frames.count == 2, "reuse released frame");
			IOSurfaceIncrementUseCount(frame->surface);
			__atomic_store_n(&control.surfaces[frame->slot].state, RAVEN_SURFACE_DISPLAYED, __ATOMIC_RELEASE);
		}
		for (unsigned i = 0; i < 128; ++i) {
			RavenDXMTDrawable *displayed = layers[i]->frames[0];
			check(IOSurfaceIsInUse(displayed->surface), "stealing metadata preserves old host surface");
			IOSurfaceDecrementUseCount(displayed->surface);
			IOSurfaceDecrementUseCount(((RavenDXMTDrawable *)layers[i]->frames[1])->surface);
			[layers[i] release];
		}

		struct layer_props props = {.device = (uint64_t)device, .width = 16, .height = 16, .format = 80};
		NSMutableArray *frames = [[NSMutableArray alloc] init];
		for (unsigned i = 0; i < RAVEN_SURFACE_SLOTS * 3; ++i) {
			RavenDXMTDrawable *frame = [[RavenDXMTDrawable alloc] initWithDevice:device window:i + 1 properties:&props];
			check(frame && frame->slot == -1, "textures do not reserve slots");
			[frames addObject:frame];
			[frame release];
			check([frame acquireSlot] && control.surfaces[frame->slot].window == i + 1, "recycle released slot");
			__atomic_store_n(&control.surfaces[frame->slot].state, RAVEN_SURFACE_AVAILABLE, __ATOMIC_RELEASE);
		}
		RavenDXMTDrawable *detached = frames[0];
		check(detached->slot == -1, "recycling detaches previous owner");
		IOSurfaceIncrementUseCount(detached->surface);
		check(![detached acquireSlot], "must not write texture still in host use");
		IOSurfaceDecrementUseCount(detached->surface);
		for (unsigned i = 0; i < RAVEN_SURFACE_SLOTS; ++i)
			check([(RavenDXMTDrawable *)frames[i] acquireSlot], "reserve outstanding write");
		check(![(RavenDXMTDrawable *)frames[RAVEN_SURFACE_SLOTS] acquireSlot], "must not steal outstanding write");
		RavenDXMTLayer *blocked = [[RavenDXMTLayer alloc] init];
		blocked->props = props;
		uint64_t pressure[2] = {(uint64_t)blocked, 123};
		const CFAbsoluteTime started = CFAbsoluteTimeGetCurrent();
		check(next_drawable(pressure) == (int32_t)0x887a000aU && !pressure[1], "nonblocking pressure result");
		check(CFAbsoluteTimeGetCurrent() - started < 1, "pool exhaustion must not stall five seconds");
		[blocked release];
		// All other host states must also remain unavailable under pressure.
		const uint32_t busy[] = {RAVEN_SURFACE_READY, RAVEN_SURFACE_IMPORTING};
		for (unsigned state = 0; state < sizeof(busy) / sizeof(*busy); ++state) {
			for (unsigned i = 0; i < RAVEN_SURFACE_SLOTS; ++i) __atomic_store_n(&control.surfaces[i].state, busy[state], __ATOMIC_RELEASE);
			check(![(RavenDXMTDrawable *)frames[RAVEN_SURFACE_SLOTS] acquireSlot], "must not steal host-owned slot");
		}
		for (unsigned i = 0; i < RAVEN_SURFACE_SLOTS; ++i) __atomic_store_n(&control.surfaces[i].state, RAVEN_SURFACE_AVAILABLE, __ATOMIC_RELEASE);
		for (unsigned i = 0; i < RAVEN_SURFACE_SLOTS; ++i) {
			control.surfaces[i].port = IOSurfaceCreateMachPort(slot_owners[i]->surface);
			check(control.surfaces[i].port != MACH_PORT_NULL, "create retiring send right");
		}
		[frames release];
		for (unsigned i = 0; i < RAVEN_SURFACE_SLOTS; ++i)
			check(control.surfaces[i].state == RAVEN_SURFACE_UNUSED && !slot_owners[i] && !control.surfaces[i].port, "retire drains slots and owners");
		uint64_t invalid[2] = {0, 123};
		check(drawable_texture(invalid) == (int32_t)0xc000000dU && !invalid[1], "null drawable texture");
		[device release];
		puts("128 live views, lazy growth/reuse, 3x pool recycling, busy-state protection and retirement PASS");
	}
	return 0;
}
