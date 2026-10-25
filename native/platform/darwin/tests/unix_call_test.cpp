#include "cpu/native_cpu.h"
#include "unix_call_shared.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <sys/mman.h>

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
		// Repeated native interrupts must not restart the call's deadline (a permanent native runloop would otherwise never time out).
		const auto timed = process.call(loaded.base() + image.entry, args, std::chrono::milliseconds(10));
		if (timed.stop != raven::NativeStop::timeout)
			throw std::runtime_error("interrupted native call escaped its deadline");
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
