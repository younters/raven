#include "desktop/shared_pixels.h"
#include "shared_surface.h"

#include <map>
#include <mutex>
#include <cstdlib>

#include <fcntl.h>
#include <new>
#include <stdexcept>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>

namespace raven {

namespace {
std::mutex surfaces_mutex;
std::map<int, IOSurfaceRef> surfaces;
}

IOSurfaceRef darwin::create_pixel_surface(uint32_t width, uint32_t height, size_t stride, size_t size)
{
	CFMutableDictionaryRef properties = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
		&kCFTypeDictionaryValueCallBacks);
	const auto set = [&](CFStringRef key, int64_t value) {
		CFNumberRef number = CFNumberCreate(nullptr, kCFNumberSInt64Type, &value);
		CFDictionarySetValue(properties, key, number);
		CFRelease(number);
	};
	set(kIOSurfaceWidth, width);
	set(kIOSurfaceHeight, height);
	set(kIOSurfaceBytesPerElement, 4);
	set(kIOSurfaceBytesPerRow, stride);
	set(kIOSurfaceAllocSize, size);
	set(kIOSurfacePixelFormat, uint32_t('BGRA'));
	IOSurfaceRef result = IOSurfaceCreate(properties);
	CFRelease(properties);
	return result;
}

IOSurfaceRef darwin::pixel_surface(int descriptor)
{
	std::lock_guard lock(surfaces_mutex);
	const auto found = surfaces.find(descriptor);
	return found == surfaces.end() ? nullptr : found->second;
}

IOSurfaceRef darwin::pixel_surface(const uint8_t *address)
{
	std::lock_guard lock(surfaces_mutex);
	for (const auto &[descriptor, surface] : surfaces)
		if (IOSurfaceGetBaseAddress(surface) == address) return surface;
	return nullptr;
}

SharedPixels::SharedPixels(size_t size, uint32_t width, uint32_t height) : SharedPixels(size)
{
	// Parallel guest threads can write pixels while the host presents. Until GDI has a producer fence,
	// retain the existing snapshot path in that mode (and for the explicit comparison backend).
	const char *present = std::getenv("RAVEN_COCOA_PRESENT");
	if ((present && std::string_view(present) == "copy") || std::getenv("RAVEN_PARALLEL")) return;
	IOSurfaceRef storage = darwin::create_pixel_surface(width, height, size_t(width) * 4, size);
	if (!storage) return;
	if (IOSurfaceGetBytesPerRow(storage) != size_t(width) * 4 || IOSurfaceGetAllocSize(storage) < size ||
		ftruncate(file, off_t(IOSurfaceGetAllocSize(storage))) != 0 ||
		IOSurfaceLock(storage, 0, nullptr) != kIOReturnSuccess) {
		CFRelease(storage);
		return;
	}
	try {
		std::lock_guard lock(surfaces_mutex);
		surfaces.emplace(file, storage);
	} catch (...) {
		IOSurfaceUnlock(storage, 0, nullptr);
		CFRelease(storage);
		throw;
	}
	munmap(mapping, length);
	mapping = static_cast<uint8_t *>(IOSurfaceGetBaseAddress(storage));
	surface = storage;
}


SharedPixels::SharedPixels(size_t size) : length(size)
{
	file = memfd_create("raven-pixels", MFD_CLOEXEC);
	if (file < 0)
		throw std::bad_alloc();
	if (ftruncate(file, off_t(size)) != 0) {
		close(file);
		throw std::bad_alloc();
	}
	void *address = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
	if (address == MAP_FAILED) {
		close(file);
		throw std::bad_alloc();
	}
	mapping = static_cast<uint8_t *>(address);
}

SharedPixels::SharedPixels(int section, uint64_t offset, size_t size) : length(size), view_offset(offset)
{
	if (offset % 4096)
		throw std::invalid_argument("section offset is not page aligned");
	// Windows section views can start at 4 KiB offsets; arm64 macOS mmap requires 16 KiB offsets.
	const size_t delta = offset % size_t((sysconf)(_SC_PAGESIZE));
	if (size > SIZE_MAX - delta)
		throw std::bad_alloc();
	file = fcntl(section, F_DUPFD_CLOEXEC, 3);
	if (file < 0)
		throw std::bad_alloc();
	void *address = mmap(nullptr, size + delta, PROT_READ | PROT_WRITE, MAP_SHARED, file, off_t(offset - delta));
	if (address == MAP_FAILED) {
		close(file);
		throw std::bad_alloc();
	}
	mapping = static_cast<uint8_t *>(address) + delta;
}

SharedPixels::~SharedPixels()
{
	if (surface) {
		{
			std::lock_guard lock(surfaces_mutex);
			surfaces.erase(file);
		}
		IOSurfaceUnlock(static_cast<IOSurfaceRef>(surface), 0, nullptr);
		CFRelease(static_cast<IOSurfaceRef>(surface));
	} else if (mapping) {
		const size_t delta = view_offset % size_t((sysconf)(_SC_PAGESIZE));
		munmap(mapping - delta, length + delta);
	}
	if (file >= 0)
		close(file);
}

}
