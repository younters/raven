// Darwin-specific contracts: resident pthreads must progress while their host frames are parked, and service operations must remain
// atomic with their instructions. No Windows libraries or scheduler are involved in this test.
#include "cpu/native_cpu.h"

#include <cstring>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>


namespace {

void check(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

uint64_t word(raven::NativeProcess &process, uint64_t address, size_t size)
{
	const auto bytes = process.read_memory(address, size);
	uint64_t value = 0;
	std::memcpy(&value, bytes.data(), size);
	return value;
}

void contracts(const std::string &bootstrap, raven::NativeMode mode)
{
	const auto timeout = std::chrono::seconds(5);
	raven::NativeProcess process({}, bootstrap, timeout, mode);
	check(process.parallel() && process.execution_descriptor() >= 0, "parallel guest has no wake descriptor");
	const size_t width = mode == raven::NativeMode::x64 ? 8 : 4;
	const auto data = process.map_memory(4096, PROT_READ | PROT_WRITE);
	const auto stack = process.map_memory(65536, PROT_READ | PROT_WRITE);
	const auto code = process.map_memory(4096, PROT_READ | PROT_WRITE);
	// Publish our barrier, wait for the other pthread, then increment the shared counter a fixed number of times.
	const std::vector<uint8_t> x64{
		0xc7, 0x02, 1, 0, 0, 0, 0x41, 0x83, 0x38, 0, 0x74, 0xfa,
		0xf0, 0x48, 0xff, 0x01, 0x49, 0xff, 0xc9, 0x75, 0xf7, 0xb8, 42, 0, 0, 0, 0xc3};
	const std::vector<uint8_t> x86{
		0x8b, 0x4c, 0x24, 4, 0x8b, 0x54, 0x24, 8, 0xc7, 0x02, 1, 0, 0, 0,
		0x8b, 0x54, 0x24, 12, 0x83, 0x3a, 0, 0x74, 0xfb, 0x8b, 0x54, 0x24, 16,
		0xf0, 0xff, 0x01, 0x4a, 0x75, 0xfa, 0xb8, 42, 0, 0, 0, 0xc3};
	process.write_memory(code, width == 8 ? x64 : x86);
	process.protect_memory(code, 4096, PROT_READ | PROT_EXEC);
	bool nested = false, started = false, serviced = false;
	raven::NativeThreadState other;
	constexpr uint64_t rounds = 1'000'000, host_increments = 1000;
	process.set_execution_wait([&](std::function<bool()> ready, auto until) {
		if (!started) {
			started = true;
			const auto main = process.save_thread(false);
			// Creation must work even when the selected guest is running.
			other = process.new_thread(stack + 65536, 0);
			process.load_thread(other);
			nested = true;
			const uint64_t arguments[]{data, data + 20, data + 16, rounds};
			const auto result = process.call(code, arguments, timeout);
			check(result.stop == raven::NativeStop::returned && result.value == 42, "second pthread did not cross the barrier");
			other = process.save_thread(false);
			process.load_thread(main);
			nested = false;
		}
		if (nested && !serviced) {
			serviced = true;
			uint64_t previous = word(process, data, width);
			for (uint64_t count = 0; count < host_increments;) {
				const uint64_t observed = process.compare_exchange(data, previous, previous + 1, width);
				if (observed == previous) {
					++count;
					++previous;
				} else {
					previous = observed;
				}
			}
			const auto temporary = process.map_memory(4096, PROT_READ | PROT_WRITE);
			const uint8_t byte = 0x5a;
			process.write_memory(temporary, {&byte, 1});
			process.protect_memory(temporary, 4096, PROT_READ);
			check(word(process, temporary, 1) == byte, "live guest memory service lost a write");
			process.unmap_memory(temporary);
		}
		while (!ready() && std::chrono::steady_clock::now() < until) {
			pollfd wake{process.execution_descriptor(), POLLIN, 0};
			::poll(&wake, 1, 10);
			process.drain_execution();
		}
	});
	const uint64_t arguments[]{data, data + 16, data + 20, rounds};
	const auto result = process.call(code, arguments, timeout);
	check(result.stop == raven::NativeStop::returned && result.value == 42 && serviced, "first pthread did not cross the barrier");
	check(word(process, data, width) == 2 * rounds + host_increments, "host and guest atomic increments lost an update");
	pollfd wake{process.execution_descriptor(), POLLIN, 0};
	check(::poll(&wake, 1, 100) == 1 && (wake.revents & POLLIN), "a guest stop did not wake the scheduler");
	process.drain_execution();
	wake.revents = 0;
	check(::poll(&wake, 1, 0) == 0, "draining the scheduler wake left the pipe readable");
	process.set_execution_wait({});
	process.stop_thread(other);
	// Reuse physical channels, but never reuse task identities: stale scheduler snapshots cannot select a different pthread.
	for (unsigned index = 0; index < 300; ++index) {
		const auto thread = process.new_thread(stack + 65536, 0);
		check(thread.task > other.task, "guest task identity was reused");
		process.stop_thread(thread);
	}
	bool rejected = false;
	try { process.load_thread(other); } catch (const std::runtime_error &) { rejected = true; }
	check(rejected, "a joined pthread was selected through a stale snapshot");
}

}

int main(int argc, char **argv)
{
	try {
		check(argc == 2, "expected the guest agent path");
		contracts(argv[1], raven::NativeMode::x64);
		contracts(argv[1], raven::NativeMode::x86);
		std::cout << "Darwin parallel pthread contracts passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
