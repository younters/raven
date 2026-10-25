#include "desktop/shared_pixels.h"

#include <fcntl.h>
#include <new>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

namespace raven {

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
	if (mapping) {
		const size_t delta = view_offset % size_t((sysconf)(_SC_PAGESIZE));
		munmap(mapping - delta, length + delta);
	}
	if (file >= 0)
		close(file);
}

}
