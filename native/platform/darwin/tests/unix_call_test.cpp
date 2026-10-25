#include "cpu/native_cpu.h"
#include "unix_call_shared.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <sys/mman.h>
#include <poll.h>

int main(int argc, char **argv)
{
	if (argc != 4)
		return 2;
	try {
		std::ifstream file(argv[2], std::ios::binary);
		std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
		raven::PEImage image(bytes);
		raven::LoadedImage loaded(image, image.image_base);
		raven::LoadedImage *images[] = {&loaded};
		raven::NativeProcess process(images, argv[1], std::chrono::seconds(5));
		const auto handle = process.load_unix_library(argv[3], 1);
		const uint64_t data = process.map_memory(4096, PROT_READ | PROT_WRITE);
		const uint64_t teb = process.map_memory(4096, PROT_READ | PROT_WRITE);
		auto context = process.context();
		context.registers.gs_base = teb;
		process.set_context(context);
		process.write_memory(teb + 0x30, {reinterpret_cast<const uint8_t *>(&teb), sizeof teb});
		const auto [stack_low, stack_high] = process.stack_bounds();
		unix_call_probe probe{process.unix_call_gate(), handle, stack_low, stack_high, teb};
		process.write_memory(data, {reinterpret_cast<const uint8_t *>(&probe), sizeof probe});
		unsigned yields = 0, samples = 0;
		process.set_time_slicing(std::chrono::milliseconds(1), [] { return true; }, [&] { ++yields; });
		process.set_guest_sampler(std::chrono::milliseconds(1), [&](uint64_t) { ++samples; });
		const uint64_t args[] = {data};
		const auto result = process.call(loaded.base() + image.entry, args, std::chrono::seconds(5));
		const auto output = process.read_memory(data, sizeof probe);
		std::memcpy(&probe, output.data(), sizeof probe);
		const auto native = process.read_memory(probe.native_allocation, 18);
		if (std::memcmp(native.data(), "native allocation", 18))
			throw std::runtime_error("host could not read a unixlib allocation");
		if (result.stop != raven::NativeStop::returned)
			std::cerr << "signal " << result.signal << " code " << result.signal_code << " rip 0x" << std::hex << result.instruction
				<< " fault 0x" << result.fault_address << " gate 0x" << probe.gate << std::dec << '\n';
		if (result.stop != raven::NativeStop::returned || result.value || probe.calls != 4 || !probe.preserved || samples <= yields)
			throw std::runtime_error("unix-call PE failed: stop " + std::to_string(int(result.stop)) + " result " +
				std::to_string(result.value) + " calls " + std::to_string(probe.calls) + " registers " + std::to_string(probe.preserved));
		std::cout << "PE -> x86_64 unixlib: GS, native stack, GPRs, XMM6-15, repeated calls and invalid handles/codes passed; "
			<< samples << " interrupts, " << yields << " scheduler yields outside native calls\n";
		if (process.parallel()) {
			const auto second_stack = process.map_memory(65536, PROT_READ | PROT_WRITE);
			const auto second_teb = process.map_memory(4096, PROT_READ | PROT_WRITE);
			process.write_memory(second_teb + 0x30, {reinterpret_cast<const uint8_t *>(&second_teb), sizeof second_teb});
			const auto second_data = process.map_memory(4096, PROT_READ | PROT_WRITE);
			unix_call_probe second{process.unix_call_gate(), handle, second_stack, second_stack + 65536, second_teb};
			process.write_memory(second_data, {reinterpret_cast<const uint8_t *>(&second), sizeof second});
			second.rendezvous = second_data + sizeof second;
			process.write_memory(second_data, {reinterpret_cast<const uint8_t *>(&second), sizeof second});
			probe.rendezvous = second.rendezvous;
			probe.calls = 0;
			process.write_memory(data, {reinterpret_cast<const uint8_t *>(&probe), sizeof probe});
			bool started = false;
			raven::NativeThreadState other;
			process.set_execution_wait([&](std::function<bool()> ready, auto until) {
				if (!started) {
					started = true;
					const auto main = process.save_thread(false);
					other = process.new_thread(second_stack + 65536, second_teb);
					process.load_thread(other);
					const uint64_t arguments[]{second_data};
					const auto result = process.call(loaded.base() + image.entry, arguments, std::chrono::seconds(5));
					if (result.stop != raven::NativeStop::returned || result.value)
						throw std::runtime_error("second pthread unix call failed");
					other = process.save_thread(false);
					process.load_thread(main);
				}
				while (!ready() && std::chrono::steady_clock::now() < until) {
					pollfd wake{process.execution_descriptor(), POLLIN, 0};
					::poll(&wake, 1, 1);
					process.drain_execution();
				}
			});
			const auto concurrent = process.call(loaded.base() + image.entry, args, std::chrono::seconds(5));
			process.set_execution_wait({});
			process.stop_thread(other);
			const auto output = process.read_memory(second_data, sizeof second);
			std::memcpy(&second, output.data(), sizeof second);
			const auto main_output = process.read_memory(data, sizeof probe);
			std::memcpy(&probe, main_output.data(), sizeof probe);
			if (!started || probe.calls != 4 || !probe.preserved || !second.preserved)
				throw std::runtime_error("parallel unix calls did not both complete");
			if (concurrent.stop != raven::NativeStop::returned || concurrent.value || second.calls != 4 ||
				second.native_thread == probe.native_thread || second.native_stack == probe.native_stack)
				throw std::runtime_error("parallel unix calls shared pthread state");
			std::cout << "concurrent pthread unix calls use distinct TSD, stacks and channels\n";
		}
		// Repeated native interrupts must not restart the call's deadline (a permanent native runloop would otherwise never time out).
		const auto timed = process.call(loaded.base() + image.entry, args, std::chrono::milliseconds(10));
		if (timed.stop != raven::NativeStop::timeout)
			throw std::runtime_error("interrupted native call escaped its deadline");
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
