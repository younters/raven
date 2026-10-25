// A repeatable transport benchmark, separate from the tests: no desktop or external Windows program is needed.
#include "cpu/native_cpu.h"
#include "nt/providers.h"

#include <sys/mman.h>
#include <unistd.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

static void check(bool value, const char *message)
{
	if (!value)
		throw std::runtime_error(message);
}

template<typename Operation> static void measure(const char *name, unsigned count, Operation operation)
{
	for (unsigned index = 0; index < 100; ++index)
		operation();
	const auto start = std::chrono::steady_clock::now();
	for (unsigned index = 0; index < count; ++index)
		operation();
	const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
	std::cout << name << " calls=" << count << " us/call=" << double(ns) / count / 1000 << '\n';
}

int main(int argc, char **argv)
{
	try {
		check(argc == 2, "bootstrap path required");
		const auto timeout = std::chrono::seconds(5);
		raven::ModuleSet modules;
		const std::array<std::string_view, 1> exports{"QueryPerformanceFrequency"};
		auto &provider = modules.add("kernel32.dll", raven::runtime_provider("kernel32.dll", 0x180000000ULL, exports));
		raven::LoadedImage *images[]{&provider.loaded};
		raven::NativeProcess process(images, argv[1], timeout);
		const auto code = process.map_memory(4096, PROT_READ | PROT_WRITE);
		// Preserve the loop count and callee in nonvolatile registers; reserve Win64 shadow space and align the stack for each call.
		const std::array<uint8_t, 31> loop{0x53, 0x41, 0x54, 0x48, 0x83, 0xec, 0x28, 0x89, 0xcb, 0x49, 0x89, 0xd4,
			0x41, 0xff, 0xd4, 0xff, 0xcb, 0x75, 0xf9, 0x48, 0x83, 0xc4, 0x28, 0x41, 0x5c, 0x5b, 0x31, 0xc0, 0xc3, 0xcc, 0xc3};
		process.write_memory(code, loop);
		process.protect_memory(code, 4096, PROT_READ | PROT_EXEC);
		const uint64_t trap = code + 29;
		process.add_api_trap(trap);
		const auto trapped = [&](unsigned count) {
			const uint64_t arguments[]{count, trap};
			auto result = process.call(code, arguments, timeout);
			unsigned calls = 0;
			while (result.stop == raven::NativeStop::api) {
				++calls;
				process.return_api(0);
				result = process.resume(timeout);
			}
			check(result.stop == raven::NativeStop::returned && calls == count, "API trap loop failed");
		};
		trapped(100);
		const auto start = std::chrono::steady_clock::now();
		trapped(5000);
		std::cout << "api-trap-loop calls=5000 us/call=" << double(std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - start).count()) / 5000 / 1000 << '\n';
		const int file = memfd_create("controller-bench", MFD_CLOEXEC);
		check(file >= 0 && ftruncate(file, 16384) == 0, "cannot create benchmark shared block");
		auto *block = static_cast<uint64_t *>(mmap(nullptr, 16384, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0));
		check(block != MAP_FAILED, "cannot map benchmark block");
		const auto shared = process.map_descriptor(file, 16384);
		close(file);
		const auto stub = provider.loaded.base() + provider.exports.at(0).address;
		int32_t displacement = 0;
		std::memcpy(&displacement, provider.loaded.data().data() + provider.exports.at(0).address + 3, 4);
		std::memcpy(provider.loaded.data().data() + provider.exports.at(0).address + 7 + displacement, &shared, 8);
		unsigned served = 0;
		process.set_served_calls(block, {stub}, 112, 85, {}, [&](uint64_t) { ++served; process.return_api(0); });
		const uint64_t arguments[]{20000, stub};
		const auto served_start = std::chrono::steady_clock::now();
		const auto result = process.call(code, arguments, timeout);
		const auto served_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - served_start).count();
		check(result.stop == raven::NativeStop::returned && served == 20000, "served call loop failed");
		std::cout << "served-loop calls=20000 us/call=" << double(served_ns) / served / 1000 << '\n';
		process.set_served_calls(nullptr, {}, 0, 0, {}, {});
		const auto memory = process.map_memory(16384, PROT_READ | PROT_WRITE);
		const std::array<uint8_t, 8> bytes{1, 2, 3, 4};
		measure("mach-read-8", 10000, [&] { process.read_memory(memory, 8); });
		measure("mach-write-8", 10000, [&] { process.write_memory(memory, bytes); });
		measure("shared-read-8", 10000, [&] { process.read_memory(shared + 4096, 8); });
		measure("shared-write-8", 10000, [&] { process.write_memory(shared + 4096, bytes); });
		measure("service-protect-pair", 2000, [&] {
			process.protect_memory(memory, 16384, PROT_READ);
			process.protect_memory(memory, 16384, PROT_READ | PROT_WRITE);
		});
		process.unmap_memory(memory);
		process.unmap_memory(shared);
		process.unmap_memory(code);
		munmap(block, 16384);
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
