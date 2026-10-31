// Private mtld3d presentation entry points over Raven's existing IOSurface transport.
#import <QuartzCore/CAMetalLayer.h>
// Reuse the pool, IOSurface slots and completion publication without registering
// a second copy of DXMT's Objective-C classes or replacing its Wine resolver.
#define RavenDXMTLayer RavenMTLD3DTransport
#define RavenDXMTDrawable RavenMTLD3DDrawable
#define macdrv_functions raven_mtld3d_macdrv_functions
#define raven_dxmt_wrap_presentation raven_mtld3d_wrap_presentation
#define raven_dxmt_control raven_mtld3d_control
#include "../guest/protocol.h"
static struct raven_control *control;
struct raven_control *raven_mtld3d_control(void) { return __atomic_load_n(&control, __ATOMIC_ACQUIRE); }
uint32_t raven_mtld3d_protocol_version(void) { return RAVEN_GUEST_VERSION; }
void raven_mtld3d_init(struct raven_control *shared) { __atomic_store_n(&control, shared, __ATOMIC_RELEASE); }
#include "../dxmt/presentation.m"

@interface RavenMTLD3DLayer : CAMetalLayer {
@public
    RavenDXMTLayer *transport;
}
@end

@interface RavenDXMTDrawable (MTLD3D)
- (id<MTLTexture>)texture;
- (CAMetalLayer *)layer;
@end
@implementation RavenDXMTDrawable (MTLD3D)
- (id<MTLTexture>)texture { return texture; }
- (CAMetalLayer *)layer { return nil; }
@end

@implementation RavenMTLD3DLayer
- (id<CAMetalDrawable>)nextDrawable
{
    uint64_t args[2] = {(uintptr_t)transport, 0};
    @synchronized(self) {
        struct layer_props props = transport->props;
        props.width = self.drawableSize.width;
        props.height = self.drawableSize.height;
        props.format = self.pixelFormat;
        uint64_t update[2] = {(uintptr_t)transport, (uintptr_t)&props};
        set_props(update);
        next_drawable(args);
        return (id<CAMetalDrawable>)(uintptr_t)args[1];
    }
}
- (void)dealloc { [transport release]; [super dealloc]; }
@end

// The view token is deliberately opaque: the Rust port never messages it as NSView.
void *raven_mtld3d_layer_create(uint64_t hwnd, void *device, uint32_t width, uint32_t height)
{
    if (!hwnd || !device || !width || !height || width > 16384 || height > 16384) return NULL;
    RavenMTLD3DLayer *layer = [[RavenMTLD3DLayer alloc] init];
    layer->transport = [[RavenDXMTLayer alloc] init];
    layer->transport->window = hwnd;
    layer->transport->props.device = (uintptr_t)device;
    layer->transport->props.width = width;
    layer->transport->props.height = height;
    layer.device = (id<MTLDevice>)device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = NO;
    layer.contentsScale = 1;
    layer.bounds = CGRectMake(0, 0, width, height);
    layer.drawableSize = CGSizeMake(width, height);
    return layer;
}
void raven_mtld3d_layer_release(void *layer) { [(id)layer release]; }
int32_t raven_mtld3d_present(void *command, void *drawable, double duration)
{
    uint64_t args[2] = {(uintptr_t)command, (uintptr_t)drawable};
    return present_common(args, duration);
}
