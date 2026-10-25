#pragma once

#include <IOSurface/IOSurface.h>
#include <cstddef>
#include <cstdint>

namespace raven::darwin {

// Borrowed references, valid while the owning SharedPixels is alive. Only window surfaces register here;
// ordinary DIB sections retain their descriptor-backed storage and section-offset semantics.
IOSurfaceRef pixel_surface(int descriptor);
IOSurfaceRef pixel_surface(const uint8_t *address);
IOSurfaceRef create_pixel_surface(uint32_t width, uint32_t height, size_t stride, size_t size);

}
