// DXMT v0.80's window/drawable ABI, implemented with public IOSurface and Metal APIs in the x86_64 process.
#include "../guest/protocol.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <mach/mach.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

extern struct raven_control *raven_dxmt_control(void);
@class RavenDXMTLayer;
@class RavenDXMTDrawable;
// Slots describe in-flight presentations, not every texture a swapchain retains.
static RavenDXMTDrawable *slot_owners[RAVEN_SURFACE_SLOTS];

// v0.80 WMTLayerProps, pack(8). The fields are ABI, not a dependency on Wine's window-driver structures.
struct layer_props {
	uint64_t device;
	double scale, width, height;
	bool opaque, sync, framebuffer_only;
	uint32_t format;
};
_Static_assert(sizeof(struct layer_props) == 40, "DXMT v0.80 layer ABI");

@interface RavenDXMTDrawable : NSObject {
@public
	IOSurfaceRef surface;
	id<MTLTexture> texture;
	int slot;
	uint64_t window;
	RavenDXMTLayer *owner; // Non-owning; used only while the swap chain is encoding a present.
}
- (bool)acquireSlot;
- (instancetype)initWithDevice:(id<MTLDevice>)device window:(uint64_t)window properties:(const struct layer_props *)props;
@end

@interface RavenDXMTLayer : NSObject {
@public
	uint64_t window;
	struct layer_props props;
	NSMutableArray *frames;
	CFAbsoluteTime next_presentation;
}
@end

@implementation RavenDXMTDrawable
- (instancetype)initWithDevice:(id<MTLDevice>)device window:(uint64_t)target properties:(const struct layer_props *)props
{
	self = [super init];
	if (!self) return nil;
	slot = -1;
	window = target;
	struct raven_control *shared = raven_dxmt_control();
	if (!shared || !isfinite(props->width) || !isfinite(props->height) || props->width < 1 || props->height < 1 ||
		props->width > 16384 || props->height > 16384 || (props->format != 70 && props->format != 80)) {
		[self release];
		return nil;
	}
	const uint32_t width = (uint32_t)props->width, height = (uint32_t)props->height;
	const uint32_t row = (width * 4 + 63) & ~63U;
	const uint32_t format = props->format == 80 ? 'BGRA' : 'RGBA';
	surface = IOSurfaceCreate((CFDictionaryRef)@{
		(NSString *)kIOSurfaceWidth: @(width), (NSString *)kIOSurfaceHeight: @(height),
		(NSString *)kIOSurfaceBytesPerElement: @4, (NSString *)kIOSurfaceBytesPerRow: @(row),
		(NSString *)kIOSurfaceAllocSize: @((uint64_t)row * height), (NSString *)kIOSurfacePixelFormat: @(format)});
	MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)props->format
		width:width height:height mipmapped:NO];
	desc.storageMode = MTLStorageModeShared;
	desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	if (surface)
		texture = [device newTextureWithDescriptor:desc iosurface:surface plane:0];
	if (!texture) {
		[self release];
		return nil;
	}
	return self;
}

- (bool)acquireSlot
{
	struct raven_control *shared = raven_dxmt_control();
	@synchronized([RavenDXMTDrawable class]) {
		if (slot >= 0) {
			struct raven_surface_frame *frame = &shared->surfaces[slot];
			const uint32_t state = __atomic_load_n(&frame->state, __ATOMIC_ACQUIRE);
			// The send right contributes to IOSurfaceIsInUse; drop it after host import.
			if ((state == RAVEN_SURFACE_AVAILABLE || state == RAVEN_SURFACE_DISPLAYED) && frame->port) {
				mach_port_deallocate(mach_task_self(), frame->port);
				frame->port = 0;
			}
			uint32_t available = RAVEN_SURFACE_AVAILABLE;
			return !IOSurfaceIsInUse(surface) && __atomic_compare_exchange_n(&frame->state, &available,
				RAVEN_SURFACE_WRITING, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
		}
		if (IOSurfaceIsInUse(surface)) return false;
		for (unsigned i = 0; i < RAVEN_SURFACE_SLOTS; ++i) {
			struct raven_surface_frame *frame = &shared->surfaces[i];
			uint32_t state = __atomic_load_n(&frame->state, __ATOMIC_ACQUIRE);
			if (state != RAVEN_SURFACE_UNUSED && state != RAVEN_SURFACE_AVAILABLE) continue;
			if (!__atomic_compare_exchange_n(&frame->state, &state, RAVEN_SURFACE_WRITING, 0,
				__ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) continue;
			// AVAILABLE acknowledges that the host has finished with the previous slot.
			// Its drawable keeps its Metal texture but obtains a new slot on its next present.
			if (slot_owners[i]) slot_owners[i]->slot = -1;
			if (frame->port) mach_port_deallocate(mach_task_self(), frame->port);
			slot_owners[i] = self;
			slot = (int)i;
			frame->port = 0;
			frame->window = window;
			frame->width = (uint32_t)IOSurfaceGetWidth(surface);
			frame->height = (uint32_t)IOSurfaceGetHeight(surface);
			frame->pixel_format = IOSurfaceGetPixelFormat(surface);
			return true;
		}
	}
	return false;
}

- (void)dealloc
{
	struct raven_control *shared = raven_dxmt_control();
	@synchronized([RavenDXMTDrawable class]) {
		if (slot >= 0 && shared) {
			struct raven_surface_frame *frame = &shared->surfaces[slot];
			const mach_port_t port = frame->port;
			__atomic_store_n(&frame->state, RAVEN_SURFACE_RETIRE, __ATOMIC_RELEASE);
			// Keep guest allocators out until the old send right is destroyed:
			// the host can publish UNUSED before this destructor resumes.
			for (unsigned wait = 0; wait < 1000 && __atomic_load_n(&frame->state, __ATOMIC_ACQUIRE) != RAVEN_SURFACE_UNUSED; ++wait)
				usleep(1000);
			if (port) mach_port_deallocate(mach_task_self(), port);
			frame->port = 0;
			slot_owners[slot] = nil;
		}
	}
	[texture release];
	if (surface) CFRelease(surface);
	[super dealloc];
}
@end

@implementation RavenDXMTLayer
- (instancetype)init
{
	self = [super init];
	if (self) {
		props = (struct layer_props){.scale = 1, .width = 1, .height = 1, .opaque = true, .format = 80};
		frames = [[NSMutableArray alloc] init];
	}
	return self;
}
- (void)dealloc
{
	[frames release];
	[super dealloc];
}
@end

static int32_t create_view(void *args)
{
	uint64_t *p = args;
	RavenDXMTLayer *layer = [[RavenDXMTLayer alloc] init];
	layer->window = p[0];
	layer->props.device = p[1];
	p[2] = p[3] = (uint64_t)layer;
	return 0;
}

static int32_t release_view(void *args)
{
	[(RavenDXMTLayer *)*(uint64_t *)args release];
	return 0;
}

static int32_t set_props(void *args)
{
	const uint64_t *p = args;
	RavenDXMTLayer *layer = (RavenDXMTLayer *)p[0];
	const struct layer_props *next = (const void *)p[1];
	if (next->width != layer->props.width || next->height != layer->props.height || next->format != layer->props.format)
		[layer->frames removeAllObjects];
	layer->props = *next;
	return 0;
}

static int32_t get_props(void *args)
{
	const uint64_t *p = args;
	*(struct layer_props *)p[1] = ((RavenDXMTLayer *)p[0])->props;
	return 0;
}

static int32_t next_drawable(void *args)
{
	uint64_t *p = args;
	RavenDXMTLayer *layer = (RavenDXMTLayer *)p[0];
	p[1] = 0;
	const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + 5;
	do {
		for (RavenDXMTDrawable *drawable in layer->frames) {
			if ([drawable acquireSlot]) {
				p[1] = (uint64_t)drawable;
				return 0;
			}
		}
		// Allocate textures lazily; slots are leased only while presenting.
		if (layer->frames.count < 3) {
			RavenDXMTDrawable *drawable = [[RavenDXMTDrawable alloc] initWithDevice:(id<MTLDevice>)layer->props.device
				window:layer->window properties:&layer->props];
			if (!drawable) return (int32_t)0xc00000bbU;
			[layer->frames addObject:drawable];
			drawable->owner = layer;
			[drawable release];
			if ([drawable acquireSlot]) {
				p[1] = (uint64_t)drawable;
				return 0;
			}
		}
		usleep(500);
	} while (CFAbsoluteTimeGetCurrent() < deadline);
	fprintf(stderr, "raven DXMT: timed out waiting for an IOSurface presentation buffer\n");
	return (int32_t)0x00000102U;
}

static int32_t drawable_texture(void *args)
{
	uint64_t *p = args;
	if (!p[0]) { p[1] = 0; return (int32_t)0xc000000dU; }
	p[1] = (uint64_t)((RavenDXMTDrawable *)p[0])->texture;
	return 0;
}

static int32_t present_common(void *args, double minimum_duration)
{
	const uint64_t *p = args;
	id<MTLCommandBuffer> command = (id<MTLCommandBuffer>)p[0];
	RavenDXMTDrawable *drawable = (RavenDXMTDrawable *)p[1];
	if (!drawable) return (int32_t)0xc000000dU;
	struct raven_control *shared = raven_dxmt_control();
	struct raven_surface_frame *frame = &shared->surfaces[drawable->slot];
	static uint64_t sequence;
	frame->sequence = __atomic_add_fetch(&sequence, 1, __ATOMIC_RELAXED);
	const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
	const CFAbsoluteTime due = minimum_duration > 0 ? fmax(now, drawable->owner->next_presentation) + minimum_duration : now;
	drawable->owner->next_presentation = due;
	[command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
		// Capturing the drawable retains its texture/IOSurface until all GPU writes have finished.
		struct raven_surface_frame *frame = &shared->surfaces[drawable->slot];
		if (completed.status == MTLCommandBufferStatusError) {
			__atomic_fetch_add(&shared->surface_errors, 1, __ATOMIC_RELAXED);
			__atomic_store_n(&frame->state, RAVEN_SURFACE_AVAILABLE, __ATOMIC_RELEASE);
		} else {
			void (^publish)(void) = ^{
				struct raven_surface_frame *ready = &shared->surfaces[drawable->slot];
				ready->port = IOSurfaceCreateMachPort(drawable->surface);
				__atomic_store_n(&ready->state, RAVEN_SURFACE_READY, __ATOMIC_RELEASE);
			};
			const double delay = due - CFAbsoluteTimeGetCurrent();
			if (delay > 0)
				dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)),
					dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), publish);
			else publish();
		}
	}];
	return 0;
}

static int32_t present(void *args) { return present_common(args, 0); }
static int32_t present_after(void *args)
{
	const double duration = ((double *)args)[2];
	if (!isfinite(duration) || duration < 0 || duration > 1) return (int32_t)0xc000000dU;
	return present_common(args, duration);
}

static int32_t set_colorspace(void *args)
{
	// The initial public transport supports SDR. HDR needs a corresponding host-layer color-space/EDR contract.
	((uint64_t *)args)[2] = 0;
	return 0;
}

static int32_t get_edr(void *args)
{
	float *values = (float *)((uint64_t *)args)[1];
	values[0] = values[1] = 1;
	return 0;
}

static int32_t display_setting(void *args)
{
	uint8_t *p = args;
	*(uint64_t *)(p + 8) = 0; // No display override; use the swap chain's SDR settings.
	((float *)(p + 32))[0] = ((float *)(p + 32))[1] = 1;
	return 0;
}

void raven_dxmt_wrap_presentation(void **table)
{
	table[47] = present;
	table[48] = present_after;
	table[66] = drawable_texture;
	table[67] = next_drawable;
	table[70] = set_props;
	table[71] = get_props;
	table[72] = create_view;
	table[73] = release_view;
	table[93] = set_colorspace;
	table[97] = get_edr;
	table[101] = display_setting;
}
