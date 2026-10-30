// SPDX-License-Identifier: MIT
#include "fex_process.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

static void check(bool value, const char *message)
{
	if (!value) throw std::runtime_error(message);
}
int main(int argc, char **argv)
{
	try {
		if (argc != 3) return 2;
		const char *root = std::getenv("RAVEN_FEX_MODULE_ROOT");
		if (!root || !std::filesystem::exists(std::string(root) + "/xtajit64.dll")) {
			std::cerr << "SKIP: provision RAVEN_FEX_MODULE_ROOT/xtajit64.dll\n";
			return 77;
		}
		std::ifstream input(argv[1], std::ios::binary);
		std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
		raven::PEImage image(bytes);
		raven::LoadedImage loaded(image, image.image_base);
		const std::string mode = argv[2];
		if (mode == "syscall") {
			const uint8_t code[]{0xb8, 0x34, 0x12, 0, 0, 0x0f, 0x05, 0x48, 0x83, 0xc0, 5, 0xc3};
			memcpy(loaded.data().data() + image.entry, code, sizeof(code));
		} else if (mode == "fault") {
			const uint8_t code[]{0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0x48, 0x8b, 0, 0x48, 0x83, 0xc0, 5, 0xc3};
			memcpy(loaded.data().data() + image.entry, code, sizeof(code));
		} else if (mode == "divide") {
			const uint8_t code[]{0x31, 0xc9, 0xb8, 1, 0, 0, 0, 0x31, 0xd2, 0xf7, 0xf1, 0xc3};
			memcpy(loaded.data().data() + image.entry, code, sizeof(code));
		} else if (mode == "breakpoint") {
			loaded.data()[image.entry] = 0xcc;
			loaded.data()[image.entry + 1] = 0xc3;
		} else check(mode == "entry", "unknown mode");
		raven::LoadedImage *images[]{&loaded};
		raven::FexProcess process(images, root);
		auto result = process.call(loaded.base() + image.entry, {});
		if (mode == "syscall") {
			check(result.stop == raven::NativeStop::syscall && result.value == 0x1234, "syscall stop lost number");
			auto context = process.context();
			check(context.registers.rip == loaded.base() + image.entry + 7, "syscall RIP not advanced");
			context.registers.rax = 37;
			process.set_context(context);
			result = process.resume();
		} else if (mode == "fault") {
			check(result.stop == raven::NativeStop::exception && result.value == 0xc0000005 && result.fault_address == 0,
				"AV stop lost exception or address");
			auto context = process.context();
			check(context.registers.rip == loaded.base() + image.entry + 10, "AV RIP imprecise");
			context.registers.rip += 3;
			context.registers.rax = 37;
			process.set_context(context);
			result = process.resume();
		} else if (mode == "divide") {
			std::cerr << "divide exception code=" << std::hex << result.value << std::dec << '\n';
			check(result.stop == raven::NativeStop::exception && result.value == 0xc0000094, "divide error misclassified");
			return 0;
		} else if (mode == "breakpoint") {
			check(result.stop == raven::NativeStop::exception && result.value == 0x80000003, "provider breakpoint not delivered");
			return 0;
		}
		check(result.stop == raven::NativeStop::returned && result.value == (mode == "entry" ? 500500 : 42), "guest result mismatch");
		std::cout << "PASS: FEX " << mode << " returned " << result.value << '\n';
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
