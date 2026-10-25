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
}
- (instancetype)initWithDevice:(id<MTLDevice>)device window:(uint64_t)window properties:(const struct layer_props *)props;
@end

@interface RavenDXMTLayer : NSObject {
@public
	uint64_t window;
	struct layer_props props;
	NSMutableArray *frames;
}
@end

@implementation RavenDXMTDrawable
- (instancetype)initWithDevice:(id<MTLDevice>)device window:(uint64_t)window properties:(const struct layer_props *)props
{
	self = [super init];
	if (!self) return nil;
	slot = -1;
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
	for (unsigned i = 0; i < RAVEN_SURFACE_SLOTS; ++i) {
		uint32_t unused = RAVEN_SURFACE_UNUSED;
		if (__atomic_compare_exchange_n(&shared->surfaces[i].state, &unused, RAVEN_SURFACE_WRITING, 0,
			__ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
			slot = (int)i;
			struct raven_surface_frame *frame = &shared->surfaces[i];
			frame->port = 0;
			frame->window = window;
			frame->width = width;
			frame->height = height;
			frame->pixel_format = format;
			__atomic_store_n(&frame->state, RAVEN_SURFACE_AVAILABLE, __ATOMIC_RELEASE);
			return self;
		}
	}
	fprintf(stderr, "raven DXMT: IOSurface pool is full\n");
	[self release];
	return nil;
}

- (void)dealloc
{
	struct raven_control *shared = raven_dxmt_control();
	if (slot >= 0 && shared) {
		struct raven_surface_frame *frame = &shared->surfaces[slot];
		const mach_port_t port = frame->port;
		__atomic_store_n(&frame->state, RAVEN_SURFACE_RETIRE, __ATOMIC_RELEASE);
		// The host acknowledges retirement before the guest destroys the port name it may be copying.
		for (unsigned wait = 0; wait < 1000 && __atomic_load_n(&frame->state, __ATOMIC_ACQUIRE) != RAVEN_SURFACE_UNUSED; ++wait)
			usleep(1000);
		if (port) mach_port_deallocate(mach_task_self(), port);
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
	struct raven_control *shared = raven_dxmt_control();
	p[1] = 0;
	while (layer->frames.count < 3) {
		RavenDXMTDrawable *frame = [[RavenDXMTDrawable alloc] initWithDevice:(id<MTLDevice>)layer->props.device
			window:layer->window properties:&layer->props];
		if (!frame) return (int32_t)0xc00000bbU;
		[layer->frames addObject:frame];
		[frame release];
	}
	const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + 5;
	do {
		for (RavenDXMTDrawable *drawable in layer->frames) {
			struct raven_surface_frame *frame = &shared->surfaces[drawable->slot];
			const uint32_t status = __atomic_load_n(&frame->state, __ATOMIC_ACQUIRE);
			// A Mach send right itself contributes to IOSurfaceIsInUse. Drop it only after the host imported or discarded the frame.
			if ((status == RAVEN_SURFACE_AVAILABLE || status == RAVEN_SURFACE_DISPLAYED) && frame->port) {
				mach_port_deallocate(mach_task_self(), frame->port);
				frame->port = 0;
			}
			uint32_t available = RAVEN_SURFACE_AVAILABLE;
			if (!IOSurfaceIsInUse(drawable->surface) && __atomic_compare_exchange_n(&frame->state, &available,
				RAVEN_SURFACE_WRITING, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
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
	p[1] = (uint64_t)((RavenDXMTDrawable *)p[0])->texture;
	return 0;
}

static int32_t present(void *args)
{
	const uint64_t *p = args;
	id<MTLCommandBuffer> command = (id<MTLCommandBuffer>)p[0];
	RavenDXMTDrawable *drawable = (RavenDXMTDrawable *)p[1];
	if (!drawable) return (int32_t)0xc000000dU;
	struct raven_control *shared = raven_dxmt_control();
	struct raven_surface_frame *frame = &shared->surfaces[drawable->slot];
	static uint64_t sequence;
	frame->sequence = __atomic_add_fetch(&sequence, 1, __ATOMIC_RELAXED);
	[command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
		// Capturing the drawable retains its texture/IOSurface until all GPU writes have finished.
		struct raven_surface_frame *frame = &shared->surfaces[drawable->slot];
		if (completed.status == MTLCommandBufferStatusError) {
			__atomic_fetch_add(&shared->surface_errors, 1, __ATOMIC_RELAXED);
			__atomic_store_n(&frame->state, RAVEN_SURFACE_AVAILABLE, __ATOMIC_RELEASE);
		} else {
			frame->port = IOSurfaceCreateMachPort(drawable->surface);
			__atomic_store_n(&frame->state, RAVEN_SURFACE_READY, __ATOMIC_RELEASE);
		}
	}];
	return 0;
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
	table[48] = present; // Minimum-duration pacing is not yet implemented by this transport.
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
