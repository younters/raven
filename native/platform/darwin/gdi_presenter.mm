#include "gdi_presenter.h"
#include "shared_surface.h"

#import <Metal/Metal.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/QuartzCore.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace raven::darwin {

struct GDIPresenter::State {
	id<MTLDevice> device;
	id<MTLCommandQueue> queue;
	id<MTLRenderPipelineState> pipeline;
	struct Frame {
		IOSurfaceRef surface = nullptr;
		id<MTLTexture> texture;
		~Frame() { texture = nil; if (surface) CFRelease(surface); }
	};
	std::array<Frame, 3> frames;
	IOSurfaceRef current = nullptr;
	IOSurfaceRef source = nullptr;
	id<MTLBuffer> buffer;
	uint32_t width = 0, height = 0;
	uint64_t rendered = 0, fallback = 0, busy = 0;
	bool pending = false;

	struct Shared {
		id<MTLDevice> device;
		id<MTLCommandQueue> queue;
		id<MTLRenderPipelineState> pipeline;
	};

	static bool enabled()
	{
		const char *mode = std::getenv("RAVEN_COCOA_PRESENT");
		return !(mode && std::string_view(mode) == "copy") && !std::getenv("RAVEN_PARALLEL");
	}

	// One device, queue and pipeline for every window of the process.
	static const Shared &shared()
	{
		static const Shared made = [] {
			Shared result{};
			if (enabled()) build(result);
			return result;
		}();
		return made;
	}

	State()
	{
		if (!enabled()) return;
		const auto &made = shared();
		device = made.device;
		queue = made.queue;
		pipeline = made.pipeline;
	}

	static void build(Shared &made)
	{
		id<MTLDevice> device = made.device = MTLCreateSystemDefaultDevice();
		if (!device) return;
		id<MTLRenderPipelineState> pipeline;
		id<MTLCommandQueue> queue;
		NSError *error = nil;
		// Buffer addressing supports Raven's exact width*4 stride even for widths not divisible by four.
		// GDI's fourth byte is unspecified; write alpha=1 rather than trusting layer.opaque to ignore it.
		NSString *shader = @"#include <metal_stdlib>\n"
			"using namespace metal;\n"
			"vertex float4 vertex_main(uint id [[vertex_id]]) {\n"
			" const float2 p[3] = {float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[id],0,1); }\n"
			"fragment float4 fragment_main(float4 p [[position]], device const uint *pixels [[buffer(0)]],\n"
			" constant uint &width [[buffer(1)]]) { uint v=pixels[uint(p.y)*width+uint(p.x)];\n"
			" return float4(float((v>>16)&255)/255.,float((v>>8)&255)/255.,float(v&255)/255.,1); }\n";
		id<MTLLibrary> library = [device newLibraryWithSource:shader options:nil error:&error];
		MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
		descriptor.vertexFunction = [library newFunctionWithName:@"vertex_main"];
		descriptor.fragmentFunction = [library newFunctionWithName:@"fragment_main"];
		descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
		if (library) pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
		if (pipeline) queue = [device newCommandQueue];
		made.queue = queue;
		made.pipeline = pipeline;
		if (!queue) std::fprintf(stderr, "raven: Metal GDI unavailable: %s; using CGImage\n", error.localizedDescription.UTF8String);
	}

	~State() {
		buffer = nil;
		if (source) CFRelease(source);
		if (std::getenv("RAVEN_COCOA_PRESENT_STATS"))
			std::fprintf(stderr, "raven GDI: %llu Metal frames, %llu fallback attempts, %llu pool stalls\n",
				(unsigned long long)rendered, (unsigned long long)fallback, (unsigned long long)busy);
	}

	void *draw(std::span<const uint8_t> pixels, uint32_t w, uint32_t h)
	{
		pending = false;
		if (!queue || pixels.size() < size_t(w) * h * 4) return nullptr;
		IOSurfaceRef input = pixel_surface(pixels.data());
		// Menus created by the desktop itself and glass-composited frames are temporary byte spans.
		// Keep the snapshot backend for those rather than disguising an upload as zero-copy.
		if (!input) { ++fallback; return nullptr; }
		if (source != input) {
			buffer = nil;
			if (source) CFRelease(source);
			CFRetain(input);
			source = input;
			buffer = [device newBufferWithBytesNoCopy:IOSurfaceGetBaseAddress(source) length:IOSurfaceGetAllocSize(source)
				options:MTLResourceStorageModeShared deallocator:nil];
		}
		if (!buffer) return nullptr;
		if (width != w || height != h) {
			for (auto &frame : frames) {
				frame.texture = nil;
				if (frame.surface) CFRelease(frame.surface);
				frame.surface = nullptr;
			}
			current = nullptr;
			width = w;
			height = h;
		}
		Frame *available = nullptr;
		for (auto &frame : frames) {
			// Never overwrite the model layer's current frame, even before CA has committed it.
			// IsInUse also protects frames retained by WindowServer and the resize crossfade.
			if (!frame.surface || (frame.surface != current && !IOSurfaceIsInUse(frame.surface))) {
				available = &frame;
				break;
			}
		}
		if (!available) {
			// Coalesce bursts without a CPU snapshot. Cocoa retries after pumping CA completions,
			// including when this is the last update (a caret blink must never be lost).
			++busy;
			pending = true;
			return current;
		}
		if (!available->surface) {
			const size_t stride = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, size_t(w) * 4);
			available->surface = create_pixel_surface(w, h, stride, stride * h);
			if (!available->surface) return nullptr;
			// Match the CGImage backend explicitly, including on wide-gamut displays. Attach once
			// per allocation: IOSurface value serialization belongs off the per-frame path.
			CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
			CFPropertyListRef profile = CGColorSpaceCopyPropertyList(space);
			IOSurfaceSetValue(available->surface, kIOSurfaceColorSpace, profile);
			CFRelease(profile);
			CGColorSpaceRelease(space);
			MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
				width:w height:h mipmapped:NO];
			descriptor.usage = MTLTextureUsageRenderTarget;
			available->texture = [device newTextureWithDescriptor:descriptor iosurface:available->surface plane:0];
		}
		if (!available->texture) return nullptr;
		IOSurfaceUnlock(source, 0, nullptr);
		id<MTLCommandBuffer> commands = [queue commandBuffer];
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = available->texture;
		pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
		[encoder setRenderPipelineState:pipeline];
		[encoder setFragmentBuffer:buffer offset:0 atIndex:0];
		[encoder setFragmentBytes:&w length:sizeof(w) atIndex:1];
		[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
		[encoder endEncoding];
		[commands commit];
		// The cooperative guest stays stopped until the GPU no longer reads its persistent bitmap.
		// WindowServer subsequently reads only our separate completed output, never guest-owned pixels.
		[commands waitUntilCompleted];
		IOSurfaceLock(source, 0, nullptr);
		if (commands.status != MTLCommandBufferStatusCompleted) return nullptr;
		++rendered;
		if (rendered == 1 && std::getenv("RAVEN_COCOA_PRESENT_STATS"))
			std::fprintf(stderr, "raven GDI: zero-copy Metal source %ux%u, stride %zu\n", w, h, IOSurfaceGetBytesPerRow(source));
		current = available->surface;
		return current;
	}
};

GDIPresenter::GDIPresenter() : state(std::make_unique<State>()) {}
void GDIPresenter::prepare() { State::shared(); }
GDIPresenter::~GDIPresenter() = default;
void *GDIPresenter::frame(std::span<const uint8_t> pixels, uint32_t width, uint32_t height)
{
	return state->draw(pixels, width, height);
}

std::span<const uint8_t> GDIPresenter::pending_pixels() const
{
	if (!state->pending || !state->source) return {};
	const auto *data = static_cast<const uint8_t *>(IOSurfaceGetBaseAddress(state->source));
	// A destroyed bitmap can outlive its window briefly in CA. Only retry live CPU-owned storage.
	if (pixel_surface(data) != state->source) return {};
	return {data, size_t(state->width) * state->height * 4};
}

}
