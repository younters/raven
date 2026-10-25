#include "desktop/shared_pixels.h"
#include "shared_surface.h"
#include "nt/firmware.h"
#include "nt/memory.h"
#include "nt/providers.h"
#include <raven_darwin_fonts.h>
#include <raven_darwin_system.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <poll.h>
#include <unistd.h>
#include <x86intrin.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

static void check(bool value, const char *message)
{
	if (!value)
		throw std::runtime_error(message);
}

static bool readable(int descriptor)
{
	pollfd event{descriptor, POLLIN, 0};
	return poll(&event, 1, 0) == 1 && (event.revents & POLLIN);
}

static void events()
{
	int descriptor = eventfd(2, EFD_NONBLOCK | EFD_CLOEXEC);
	check(descriptor >= 0 && (fcntl(descriptor, F_GETFD) & FD_CLOEXEC), "eventfd flags differ");
	const uint64_t three = 3, zero = 0, maximum = UINT64_MAX - 1, one = 1;
	uint64_t value = 0;
	check(write(descriptor, &three, 8) == 8 && readable(descriptor) && read(descriptor, &value, 16) == 8 && value == 5 &&
		!readable(descriptor), "eventfd did not return and drain the accumulated count");
	check(read(descriptor, &value, 8) == -1 && errno == EAGAIN, "empty nonblocking eventfd did not return EAGAIN");
	check(write(descriptor, &zero, 8) == 8 && !readable(descriptor), "zero eventfd write caused readiness");
	check(write(descriptor, &maximum, 8) == 8 && write(descriptor, &one, 8) == -1 && errno == EAGAIN,
		"eventfd counter overflow succeeded");
	check(eventfd_read(descriptor, &value) == 0 && value == maximum, "eventfd overflow changed the count");
	value = UINT64_MAX;
	check(write(descriptor, &value, 8) == -1 && errno == EINVAL && read(descriptor, &value, 7) == -1 && errno == EINVAL,
		"eventfd invalid value or short buffer succeeded");
	std::array<std::thread, 4> writers;
	std::atomic<bool> failed{false};
	for (auto &writer : writers)
		writer = std::thread([&] {
			for (unsigned index = 0; index < 10000; ++index)
				if (eventfd_write(descriptor, 1))
					failed = true;
		});
	for (auto &writer : writers)
		writer.join();
	check(!failed && eventfd_read(descriptor, &value) == 0 && value == 40000 && !readable(descriptor),
		"concurrent eventfd wakes were lost or filled the FIFO");
	close(descriptor);
	descriptor = eventfd(2, EFD_NONBLOCK | EFD_SEMAPHORE);
	check(eventfd_read(descriptor, &value) == 0 && value == 1 && readable(descriptor) &&
		eventfd_read(descriptor, &value) == 0 && value == 1 && !readable(descriptor), "eventfd semaphore readiness differs");
	close(descriptor);
	descriptor = eventfd(0, EFD_CLOEXEC);
	std::thread writer([=] { usleep(10000); eventfd_write(descriptor, 7); });
	const bool read = eventfd_read(descriptor, &value) == 0 && value == 7;
	writer.join();
	check(read, "blocking eventfd read failed");
	close(descriptor);
	check(eventfd(0, 0x100) == -1 && errno == EINVAL, "eventfd accepted unknown flags");
}

static void pixels()
{
	const int descriptor = memfd_create("darwin-pixels-test", MFD_CLOEXEC);
	check(descriptor >= 0 && ftruncate(descriptor, 65536) == 0, "cannot create shared pixel fixture");
	auto *host = static_cast<uint8_t *>(mmap(nullptr, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0));
	check(host != MAP_FAILED, "cannot map shared pixel fixture");
	for (uint64_t offset : {4096, 8192, 12288, 16384}) {
		raven::SharedPixels view(descriptor, offset, 20000);
		check(view.offset() == offset && view.size() == 20000, "shared pixel view geometry differs");
		view.data()[0] = 17;
		view.data()[19999] = 23;
		check(host[offset] == 17 && host[offset + 19999] == 23, "shared pixel view used the wrong file offset");
	}
	munmap(host, 65536);
	close(descriptor);
}

static void guest_memory(const char *bootstrap)
{
	raven::ModuleSet modules;
	const std::array<std::string_view, 1> names{"GetLastError"};
	auto &image = modules.add("kernel32.dll", raven::runtime_provider("kernel32.dll", 0x180000000ULL, names));
	raven::LoadedImage *images[]{&image.loaded};
	raven::NativeProcess process(images, bootstrap, std::chrono::seconds(2));
	raven::NativeEnvironment environment(process, image.loaded.base(), 1, 1);
	raven::NativeMemory memory(process, environment);
	check(!memory.query(image.loaded.base()), "an image unknown to the allocator was reported free");
	const auto [low, high] = process.stack_bounds();
	check(memory.query(low).has_value() && high > low && !memory.query(low - 4096), "stack or guard-page query differs");
	const auto allocation = memory.allocate(0, 4096, 0x103000, 4);
	check(!allocation.status && memory.query(allocation.address).has_value() && !memory.free(allocation.address, 0, 0x8000).status,
		"supported MEM_TOP_DOWN allocation or release failed");
	const int file = memfd_create("shared-code-test", MFD_CLOEXEC);
	check(file >= 0 && ftruncate(file, 16384) == 0, "cannot create shared executable fixture");
	const auto code = process.map_descriptor(file, 16384);
	close(file);
	std::array<uint8_t, 6> instructions{0xb8, 42, 0, 0, 0, 0xc3};
	process.write_memory(code, instructions);
	process.protect_memory(code, 16384, PROT_READ | PROT_WRITE | PROT_EXEC);
	check(process.call(code, {}, std::chrono::seconds(1)).value == 42, "shared executable code did not run");
	instructions[1] = 43;
	process.write_memory(code, instructions);
	const auto changed = process.call(code, {}, std::chrono::seconds(1));
	check(changed.stop == raven::NativeStop::returned && changed.value == 43, "shared executable write retained a stale Rosetta translation");
	process.unmap_memory(code);
	// Odd widths exercise the tight GDI stride, including a 4 KiB tail on a 16 KiB host.
	for (uint32_t width : {1U, 17U, 801U}) {
		const size_t size = (size_t(width) * 7 * 4 + 4095) & ~size_t(4095);
		raven::SharedPixels pixels(size, width, 7);
		const auto surface = raven::darwin::pixel_surface(pixels.descriptor());
		const bool copy = std::getenv("RAVEN_COCOA_PRESENT") && std::string_view(std::getenv("RAVEN_COCOA_PRESENT")) == "copy";
		check(copy ? !surface : surface && raven::darwin::pixel_surface(pixels.data()) == surface &&
			IOSurfaceGetBytesPerRow(surface) == size_t(width) * 4, "window IOSurface stride or registry differs");
		const auto mapped = process.map_shared(pixels.descriptor(), size);
		pixels.data()[size - 1] = 0x73;
		check(process.read_memory(mapped + size - 1, 1)[0] == 0x73, "host IOSurface alias is not coherent");
		// x64: mov byte ptr [rcx], 0x51; movzx eax, byte ptr [rcx]; ret. Execute in the
		// guest so a host alias accidentally pointing at the original file cannot pass.
		const auto writer = process.map_memory(4096, PROT_READ | PROT_WRITE | PROT_EXEC);
		const std::array<uint8_t, 7> write{0xc6, 0x01, 0x51, 0x0f, 0xb6, 0x01, 0xc3};
		process.write_memory(writer, write);
		const std::array<uint64_t, 1> args{mapped + size - 1};
		check(process.call(writer, args, std::chrono::seconds(1)).value == 0x51 && pixels.data()[size - 1] == 0x51,
			"guest did not write directly into the IOSurface");
		process.unmap_memory(writer);
		process.unmap_memory(mapped);
	}
}

int main(int argc, char **argv)
{
	try {
		check(argc == 2, "bootstrap path required");
		events();
		pixels();
		guest_memory(argv[1]);
		uint64_t memory = 0;
		size_t size = sizeof memory;
		check(!sysctlbyname("hw.memsize", &memory, &size, nullptr, 0) && raven::smbios_memory(raven_darwin::smbios_table()) == memory / 1024,
			"SMBIOS installed memory differs from the host");
		check(!raven_darwin::cpu_brand().empty() && !raven_darwin::wireless_interface("lo0"), "host identity or loopback media differs");
		const auto roots = raven_darwin::font_roots();
		check(std::find(roots.begin(), roots.end(), "/System/Library/Fonts") != roots.end() && !raven_darwin::font_files().empty(),
			"macOS system fonts were not found");
		const auto start = std::chrono::steady_clock::now();
		const auto before = __rdtsc();
		usleep(20000);
		const auto after = __rdtsc();
		const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
		check(after > before && after - before > uint64_t(elapsed / 2) && after - before < uint64_t(elapsed * 2),
			"host TSC is not the guest's monotonic nanosecond counter");
		std::cout << "Darwin event counters, shared pixel offsets, SMBIOS, host identity, fonts and TSC passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
