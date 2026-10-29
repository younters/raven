// Exercise the real x86_64 allocator without windows or captured pixels.
#include "presentation.m"
#include <pthread.h>
#include <stdlib.h>

static struct raven_control control;
static bool done;
struct raven_control *raven_dxmt_control(void) { return &control; }

static void check(bool ok, const char *message)
{
	if (!ok) { fprintf(stderr, "%s\n", message); exit(1); }
}

static void *acknowledge(void *unused)
{
	(void)unused;
	while (!__atomic_load_n(&done, __ATOMIC_ACQUIRE)) {
		for (unsigned i = 0; i < RAVEN_SURFACE_SLOTS; ++i) {
			uint32_t retire = RAVEN_SURFACE_RETIRE;
			__atomic_compare_exchange_n(&control.surfaces[i].state, &retire, RAVEN_SURFACE_UNUSED, 0,
				__ATOMIC_RELEASE, __ATOMIC_RELAXED);
		}
		usleep(1000);
	}
	return NULL;
}

int main(void)
{
	@autoreleasepool {
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (!device) return 77;
		pthread_t host;
		check(!pthread_create(&host, NULL, acknowledge, NULL), "start retirement worker");
		RavenDXMTLayer *layers[24];
		for (unsigned i = 0; i < 24; ++i) {
			layers[i] = [[RavenDXMTLayer alloc] init];
			layers[i]->window = i + 1;
			layers[i]->props.device = (uint64_t)device;
			layers[i]->props.width = layers[i]->props.height = 16;
			uint64_t args[2] = {(uint64_t)layers[i], 0};
			check(!next_drawable(args) && args[1] && layers[i]->frames.count == 1, "lazy first drawable");
			RavenDXMTDrawable *frame = (id)args[1];
			__atomic_store_n(&control.surfaces[frame->slot].state, RAVEN_SURFACE_DISPLAYED, __ATOMIC_RELEASE);
		}
		for (unsigned i = 0; i < 24; ++i) {
			uint64_t args[2] = {(uint64_t)layers[i], 0};
			check(!next_drawable(args) && layers[i]->frames.count == 2, "grow only when first frame busy");
			RavenDXMTDrawable *frame = (id)args[1];
			__atomic_store_n(&control.surfaces[frame->slot].state, RAVEN_SURFACE_AVAILABLE, __ATOMIC_RELEASE);
			control.surfaces[frame->slot].port = IOSurfaceCreateMachPort(frame->surface);
			check(control.surfaces[frame->slot].port != MACH_PORT_NULL, "create drawable send right");
			check(!next_drawable(args) && args[1] == (uint64_t)frame && layers[i]->frames.count == 2, "reuse released frame");
		}
		for (unsigned i = 0; i < 24; ++i) [layers[i] release];

		struct layer_props props = {.device = (uint64_t)device, .width = 16, .height = 16, .format = 80};
		NSMutableArray *frames = [[NSMutableArray alloc] init];
		for (unsigned i = 0; i < RAVEN_SURFACE_SLOTS * 3; ++i) {
			RavenDXMTDrawable *frame = [[RavenDXMTDrawable alloc] initWithDevice:device window:i + 1 properties:&props];
			check(frame && frame->slot == -1, "textures do not reserve slots");
			[frames addObject:frame]; [frame release];
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
		// All other host states must also remain unavailable under pressure.
		const uint32_t busy[] = {RAVEN_SURFACE_READY, RAVEN_SURFACE_IMPORTING, RAVEN_SURFACE_DISPLAYED};
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
		__atomic_store_n(&done, true, __ATOMIC_RELEASE);
		pthread_join(host, NULL);
		[device release];
		puts("24 live views, lazy growth/reuse, 3x pool recycling, busy-state protection and retirement PASS");
	}
	return 0;
}
