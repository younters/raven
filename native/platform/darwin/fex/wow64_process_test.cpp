// SPDX-License-Identifier: MIT
#include "fex_process.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cstring>
#include <sys/mman.h>

static void check(bool value, const char *message)
{
	if (!value) throw std::runtime_error(message);
}
int main(int argc, char **argv)
{
	try {
		if (argc != 3) return 2;
		const char *root = std::getenv("RAVEN_FEX_MODULE_ROOT");
		if (!root || !std::filesystem::exists(std::string(root) + "/xtajit.dll")) throw std::runtime_error("M4 requires RAVEN_FEX_MODULE_ROOT/xtajit.dll");
		std::ifstream file(argv[1], std::ios::binary);
		std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
		const std::string mode = argv[2];
		if (mode == "range-laa") {
			uint32_t pe; memcpy(&pe, bytes.data() + 0x3c, 4);
			bytes[pe + 22] |= 0x20;
		}
		raven::PEImage image(bytes);
		check(!image.is_64, "requires PE32");
		raven::LoadedImage loaded(image, image.image_base);
		raven::LoadedImage *images[]{&loaded};
		raven::FexProcess process(images, root, image.stack_reserve, raven::NativeMode::x86);
		if (mode == "teb-switch") {
			const auto code = process.map_memory(16384, PROT_READ | PROT_WRITE | PROT_EXEC, 0);
			const uint8_t load[] = {0x64, 0xa1, 0x18, 0, 0, 0, 0xc3};
			process.write_memory(code, load);
			uint64_t tebs[2];
			for (auto &teb : tebs) {
				teb = process.map_memory(16384, PROT_READ | PROT_WRITE, 0);
				process.write_memory(teb + 0x18, {reinterpret_cast<const uint8_t *>(&teb), 4});
			}
			for (unsigned i = 0; i < 100; ++i) {
				process.set_teb(tebs[i % 2]);
				const auto result = process.call(code, {});
				check(result.stop == raven::NativeStop::returned && result.value == tebs[i % 2],
					"worker retained previous guest TEB");
			}
			std::cout << "PASS: alternating guest TEB on one resident worker\n";
			return 0;
		}
		if (mode == "internal") asm volatile("brk #0");
		if (mode == "range" || mode == "range-laa") {
			const bool laa = mode == "range-laa";
			check(process.memory_available(0x7ffec000, 16384), "default top page missing");
			check(process.memory_available(0x80000000, 16384) == laa, "LAA policy mismatch");
			check(process.memory_available(0xfffec000, 16384) == laa, "LAA top page mismatch");
			check(!process.memory_available(0xffff0000, 16384), "last 64K must stay reserved");
			check(!process.memory_available(0x7ffec000, laa ? SIZE_MAX : 32768), "overflow or limit crossing accepted");
			bool rejected = false;
			try { process.map_memory(16384, PROT_READ | PROT_WRITE, laa ? 0xffff0000 : 0x80000000); }
			catch (const std::runtime_error &) { rejected = true; }
			check(rejected, "fixed allocation ignored user limit");
			if (laa) check(process.map_memory(16384, PROT_READ | PROT_WRITE, 0xfffec000) == 0xfffec000, "LAA high allocation failed");
			return 0;
		}
		if (mode == "alignment") {
			for (size_t count : {0, 1, 2, 5}) {
				std::vector<uint64_t> args(count, 17);
				const auto result = process.call(loaded.base() + image.entry, args);
				check(result.stop == raven::NativeStop::returned && result.value == 500500, "unaligned WoW64 call stack");
			}
			return 0;
		}
		const auto code = process.map_memory(16384, PROT_READ | PROT_WRITE | PROT_EXEC, 0);
		check(code && code < 0x80000000, "allocation escaped guest address space");
		std::vector<uint8_t> instructions;
		if (mode == "stdcall" || mode == "cdecl") {
			instructions = {0x6a, 23, 0x6a, 17, 0xe8, 3, 0, 0, 0, 0x03, 0xc2, 0xc3, 0xcc};
			if (mode == "cdecl") { instructions.insert(instructions.begin() + 9, {0x83, 0xc4, 8}); instructions[5] = 6; }
			process.add_api_trap(code + instructions.size() - 1, mode == "stdcall" ? 8 : 0);
		} else if (mode == "fs") instructions = {0x31, 0xc9, 0x8c, 0xe1, 0x83, 0xf9, 0x53, 0x75, 7, 0x64, 0xa1, 0x18, 0, 0, 0, 0xc3, 0x31, 0xc0, 0xc3};
		else if (mode == "fault") instructions = {0x31, 0xc0, 0x8b, 0, 0x83, 0xc0, 5, 0xc3};
		else if (mode == "timeout") instructions = {0xeb, 0xfe, 0xc3};
		else if (mode == "host-smc") instructions = {0xb8, 1, 0, 0, 0, 0xc3};
		else if (mode == "guest-smc") {
			instructions = {0xc6, 5, 0, 0, 0, 0, 42, 0xeb, 0, 0xb8, 1, 0, 0, 0, 0xc3};
			const uint32_t target = uint32_t(code + 10); memcpy(instructions.data() + 2, &target, 4);
		} else throw std::runtime_error("unknown contract");
		process.write_memory(code, instructions);
		uint64_t expected = 42;
		if (mode == "fs") {
			const auto teb = process.map_memory(16384, PROT_READ | PROT_WRITE, 0);
			const uint32_t self = uint32_t(teb);
			process.write_memory(teb + 0x18, {reinterpret_cast<const uint8_t *>(&self), 4});
			process.set_teb(teb);
			expected = teb;
		}
		auto result = process.call(code, {}, std::chrono::milliseconds(mode == "timeout" ? 30 : 5000));
		if (mode == "stdcall" || mode == "cdecl") {
			check(result.stop == raven::NativeStop::api, "missing PE32 API trap");
			check(process.api_arguments(2) == std::vector<uint64_t>{17, 23}, "wrong PE32 argument words");
			const auto before = process.save_thread(true);
			bool rejected = false;
			try { process.callback(code, std::vector<uint64_t>(129), std::chrono::seconds(5)); }
			catch (const std::runtime_error &) { rejected = true; }
			check(rejected && process.save_thread(true).stack_top == before.stack_top, "callback exception lost stack ceiling");
			process.load_thread(before);
			auto nested = process.callback(loaded.base() + image.entry, {}, std::chrono::seconds(5));
			check(nested.stop == raven::NativeStop::returned && nested.value == 500500, "nested WoW64 callback failed");
			process.return_api(0x0000000500000025ULL);
			result = process.resume(std::chrono::seconds(5));
		} else if (mode == "fault") {
			check(result.stop == raven::NativeStop::exception && result.value == 0xc0000005 && result.fault_address == 0 && result.instruction == code + 2, "imprecise WoW64 access violation");
			auto context = process.context(); context.registers.rax = 37; context.registers.rip += 2;
			process.set_context(context); result = process.resume(std::chrono::seconds(5));
		} else if (mode == "timeout") {
			check(result.stop == raven::NativeStop::timeout, "WoW64 loop did not stop");
			auto context = process.context(); context.registers.rax = 42; context.registers.rip = code + 2;
			process.set_context(context); result = process.resume(std::chrono::seconds(5));
		} else if (mode == "host-smc") {
			check(result.stop == raven::NativeStop::returned && result.value == 1, "initial code failed");
			process.protect_memory(code, 16384, PROT_READ | PROT_WRITE | PROT_EXEC);
			const uint8_t value = 42; process.write_memory(code + 1, {&value, 1});
			result = process.call(code, {}, std::chrono::seconds(5));
		}
		check(result.stop == raven::NativeStop::returned && result.value == expected, "wrong WoW64 completion");
		std::cout << "PASS: WoW64 " << mode << "\n";
		return 0;
	} catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
