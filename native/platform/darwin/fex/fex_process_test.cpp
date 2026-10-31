// SPDX-License-Identifier: MIT
#include "fex_process.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
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
		if (!root || !*root || !std::filesystem::exists(std::string(root) + "/xtajit64.dll")) {
			std::cerr << "SKIP: provision RAVEN_FEX_MODULE_ROOT/xtajit64.dll\n";
			return std::getenv("RAVEN_FEX_REQUIRED") ? 1 : 77;
		}
		std::ifstream input(argv[1], std::ios::binary);
		std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
		raven::PEImage image(bytes);
		raven::LoadedImage loaded(image, image.image_base);
		const std::string mode = argv[2];
		bool matrix = mode.starts_with("matrix-");
		bool expected_fault = false;
		size_t divide_offset = 0, divide_length = 0;
		uint64_t quotient_register = 0, remainder_register = 0;
		uint64_t fault_rax = 0, fault_rdx = 0, fault_rcx = 0;
		constexpr uint64_t fault_flags = 0xed7; // OF/DF/SF/ZF/AF/PF/CF set, IF and fixed bit retained.
		if (matrix) {
			const bool sign = mode[7] == 's';
			const auto dash = mode.find('-', 8);
			const unsigned bits = unsigned(std::stoul(mode.substr(8, dash - 8)));
			const auto kind = mode.substr(dash + 1);
			check(bits == 8 || bits == 16 || bits == 32 || bits == 64, "bad divide width");
			const uint64_t min = 1ULL << (bits - 1);
			uint64_t low = 10, high = 0, divisor = 2;
			if (kind == "zero" || kind == "fold-zero") { divisor = 0; expected_fault = true; }
			else if (kind == "overflow" || kind == "fold-overflow") {
				expected_fault = true;
				if (sign) { low = uint64_t(0) - min; high = UINT64_MAX; divisor = UINT64_MAX; }
				else { low = 0; high = 1; divisor = 1; if (bits == 8) low = 0x100; }
			} else if (kind == "wide-valid") { low = 0; high = 1; divisor = 3; if (bits == 8) low = 0x100; }
			else if (kind == "positive-overflow") { low = min; divisor = 1; expected_fault = true; }
			else if (kind == "negative-limit") { low = uint64_t(0) - min; high = UINT64_MAX; divisor = 1; }
			else if (kind == "positive-limit") { low = min - 1; divisor = 1; }
			else if (kind == "wide-overflow") { low = 0; high = min; divisor = 1; expected_fault = true; }
			else check(kind == "valid" || kind == "fold-valid", "bad divide case");
			std::vector<uint8_t> code;
			auto imm = [&](uint8_t op, uint64_t value) { code.push_back(0x48); code.push_back(op); for (unsigned i = 0; i < 8; ++i) code.push_back(uint8_t(value >> (i * 8))); };
			imm(0xb8, low); imm(0xba, high); imm(0xb9, divisor);
			if (kind.starts_with("fold-")) {
				if (sign) { if (bits == 64) code.push_back(0x48); code.push_back(0x99); }
				else { code.push_back(0x31); code.push_back(0xd2); }
			}
			fault_rax = low; fault_rdx = high; fault_rcx = divisor;
			if (kind.starts_with("fold-"))
				fault_rdx = sign ? (bits == 64 ? uint64_t(int64_t(low) >> 63) : uint64_t(uint32_t(int32_t(low) >> 31))) : 0;
			code.insert(code.end(), {0x68, 0xd7, 0x0e, 0, 0, 0x9d}); // push flags; popfq
			divide_offset = code.size();
			if (bits == 16) code.push_back(0x66);
			if (bits == 64) code.push_back(0x48);
			code.push_back(bits == 8 ? 0xf6 : 0xf7); code.push_back(sign ? 0xf9 : 0xf1);
			divide_length = code.size() - divide_offset;
			if (!expected_fault) {
				const uint64_t mask = bits == 64 ? UINT64_MAX : (1ULL << bits) - 1;
				__uint128_t dividend = bits == 8 ? low & 0xffff : ((__uint128_t(high & mask) << bits) | (low & mask));
				uint64_t q, r;
				if (sign) {
					const auto shift = 128 - (bits == 8 ? 16 : bits * 2);
					const __int128_t signed_dividend = __int128_t(dividend << shift) >> shift;
					const int64_t signed_divisor = int64_t(divisor << (64 - bits)) >> (64 - bits);
					q = uint64_t(signed_dividend / signed_divisor); r = uint64_t(signed_dividend % signed_divisor);
				} else { q = uint64_t(dividend / (divisor & mask)); r = uint64_t(dividend % (divisor & mask)); }
				quotient_register = bits == 8 ? (low & ~0xffffULL) | (q & 0xff) | ((r & 0xff) << 8) :
					bits == 16 ? (low & ~mask) | (q & mask) : q & mask;
				remainder_register = bits == 8 ? high : bits == 16 ? (high & ~mask) | (r & mask) : r & mask;
			}
			// Observe quotient/remainder through a real syscall before completing.
			code.insert(code.end(), {0x48, 0x89, 0xc3, 0x48, 0x89, 0xd6, 0xb8, 0x35, 0x12, 0, 0, 0x0f, 0x05});
			// Recovery/valid completion has its own deterministic result.
			code.insert(code.end(), {0xb8, 42, 0, 0, 0, 0xc3});
			memcpy(loaded.data().data() + image.entry, code.data(), code.size());
		} else if (mode == "selfmodify") {
			const uint8_t code[]{0xb8, 1, 0, 0, 0, 0xc3};
			memcpy(loaded.data().data() + image.entry, code, sizeof(code));
		} else if (mode == "timeout" || mode == "wait-unwind") {
			loaded.data()[image.entry] = 0xeb; loaded.data()[image.entry + 1] = 0xfe; loaded.data()[image.entry + 2] = 0xc3;
		} else if (mode == "ud2") {
			loaded.data()[image.entry] = 0x0f; loaded.data()[image.entry + 1] = 0x0b;
		} else if (mode == "internal") {
			// Tested in an isolated child: a host BRK must never become a guest exception.
		} else if (mode == "debug-print") {
			const uint8_t code[]{0xb8, 1, 0, 0, 0, 0xcd, 0x2d, 0xcc, 0xc3};
			memcpy(loaded.data().data() + image.entry, code, sizeof(code));
		} else if (mode == "unaligned") {
			const uint8_t code[]{0xb8, 42, 0, 0, 0, 0x48, 0x89, 0x44, 0x24, 0xf4,
				0x48, 0x8b, 0x44, 0x24, 0xf4, 0xc3};
			memcpy(loaded.data().data() + image.entry, code, sizeof(code));
		} else if (mode == "nt-service") {
			// mov r10,rcx; mov eax,1234h; syscall; ret
			const uint8_t code[]{0x49, 0x89, 0xca, 0xb8, 0x34, 0x12, 0, 0, 0x0f, 0x05, 0xc3};
			memcpy(loaded.data().data() + image.entry, code, sizeof(code));
		} else if (mode == "syscall") {
			const uint8_t code[]{0xb8, 0x34, 0x12, 0, 0, 0x0f, 0x05, 0x48, 0x83, 0xc0, 5, 0xc3};
			memcpy(loaded.data().data() + image.entry, code, sizeof(code));
		} else if (mode == "fault") {
			const uint8_t code[]{0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0x48, 0x8b, 0, 0x48, 0x83, 0xc0, 5, 0xc3};
			memcpy(loaded.data().data() + image.entry, code, sizeof(code));
		} else if (mode == "divide") {
			const uint8_t code[]{0x31, 0xc9, 0xb8, 1, 0, 0, 0, 0x31, 0xd2, 0x68, 0xd7, 0x0e, 0, 0, 0x9d, 0xf7, 0xf1, 0xc3};
			memcpy(loaded.data().data() + image.entry, code, sizeof(code));
		} else if (mode == "breakpoint") {
			loaded.data()[image.entry] = 0xcc;
			loaded.data()[image.entry + 1] = 0xc3;
		} else check(mode == "entry" || mode == "lifetime" || mode == "served-malformed" || mode == "teb-switch", "unknown mode");
		raven::LoadedImage *images[]{&loaded};
		if (mode == "lifetime") {
			auto second_rejected = [&] {
				try { raven::FexProcess second(images, root); }
				catch (const std::runtime_error &error) {
					check(std::string(error.what()).find("second FexProcess") != std::string::npos, "second process failed for the wrong reason");
					return;
				}
				throw std::runtime_error("second FexProcess was accepted");
			};
			{
				raven::FexProcess first(images, root);
				second_rejected();
				const auto result = first.call(loaded.base() + image.entry, {});
				check(result.stop == raven::NativeStop::returned && result.value == 500500, "rejected second process damaged first");
			}
			second_rejected();
			std::cout << "PASS: second process rejected during and after controller lifetime\n";
			return 0;
		}
		raven::FexProcess process(images, root);
		if (mode == "teb-switch") {
			const auto code = process.map_memory(16384, PROT_READ | PROT_WRITE | PROT_EXEC, 0);
			const uint8_t load[] = {0x65, 0x48, 0x8b, 0x04, 0x25, 0x30, 0, 0, 0, 0xc3};
			process.write_memory(code, load);
			uint64_t tebs[2];
			for (auto &teb : tebs) {
				teb = process.map_memory(16384, PROT_READ | PROT_WRITE, 0);
				process.write_memory(teb + 0x30, {reinterpret_cast<const uint8_t *>(&teb), 8});
				const auto peb = process.map_memory(16384, PROT_READ | PROT_WRITE, 0);
				process.write_memory(teb + 0x60, {reinterpret_cast<const uint8_t *>(&peb), 8});
			}
			for (unsigned i = 0; i < 100; ++i) {
				auto context = process.context();
				context.registers.gs_base = tebs[i % 2];
				process.set_context(context);
				const auto result = process.call(code, {}, std::chrono::milliseconds(1000));
				check(result.stop == raven::NativeStop::returned && result.value == tebs[i % 2],
					"worker retained previous guest TEB");
			}
			std::cout << "PASS: alternating guest TEB on one resident worker\n";
			return 0;
		}
		if (mode == "debug-print") process.add_api_trap(loaded.base() + image.entry + 32, 0);
		if (mode == "wait-unwind") {
			process.set_execution_wait([](auto, auto) { throw std::runtime_error("wait sentinel"); });
			bool caught = false;
			try { process.call(loaded.base() + image.entry, {}); }
			catch (const std::runtime_error &error) { caught = std::string(error.what()) == "wait sentinel"; }
			check(caught && process.stopped(), "throwing wait left worker running");
			process.set_execution_wait({});
			auto context = process.context();
			context.registers.rip = loaded.base() + image.entry + 2;
			context.registers.rax = 42;
			process.set_context(context);
			const auto result = process.resume();
			check(result.stop == raven::NativeStop::returned && result.value == 42, "unwound worker cannot resume");
			std::cout << "PASS: throwing execution wait pauses worker and permits repair\n";
			return 0;
		}
		if (mode == "served-malformed") {
			try { process.set_served_calls(nullptr, {loaded.base() + image.entry}, 112, 85,
				{loaded.base() + image.entry + 109}, {}, {}); }
			catch (const std::runtime_error &error) {
				check(std::string(error.what()).find("served stub encoding") != std::string::npos, "malformed served stub failed for wrong reason");
				std::cout << "PASS: malformed real served stub rejected before execution\n";
				return 0;
			}
			throw std::runtime_error("malformed served stub was silently skipped");
		}
		if (mode == "internal") asm volatile("brk #0");
		auto result = process.call(loaded.base() + image.entry, {}, std::chrono::milliseconds(mode == "timeout" ? 20 : 5000));
		if (mode == "debug-print") {
			check(result.stop == raven::NativeStop::fault && result.signal == SIGSEGV && result.signal_code == 0x80 && result.value == 1,
				"INT 2D did not normalize canonical debug print");
			process.complete_debug_print(result.instruction);
			result = process.resume();
			check(result.stop == raven::NativeStop::returned && result.value == 0, "debug print did not return");
			std::cout << "PASS: canonical INT 2D completion\n";
			return 0;
		}
		if (mode == "nt-service") {
			check(result.stop == raven::NativeStop::syscall && result.value == 0x1234, "NT syscall number lost");
			bool rejected = false;
			try { process.enter_nt_service(result.instruction, 0x1235); }
			catch (const std::runtime_error &) { rejected = true; }
			check(rejected, "NT service accepted wrong captured number");
			auto reject = [&](uint64_t instruction) {
				bool failed = false;
				try { process.enter_nt_service(instruction, 0x1234); }
				catch (const std::runtime_error &) { failed = true; }
				check(failed, "NT service accepted invalid boundary");
			};
			reject(result.instruction + 1);
			const auto page = result.instruction & ~uint64_t(4095);
			process.protect_image(page, 4096, PROT_READ | PROT_WRITE);
			reject(result.instruction); // readable but not executable
			const uint8_t nop = 0x90, ret = 0xc3;
			process.write_memory(result.instruction, {&nop, 1});
			process.protect_image(page, 4096, PROT_READ | PROT_EXEC);
			reject(result.instruction); // syscall without its third (ret) byte
			process.protect_image(page, 4096, PROT_READ | PROT_WRITE);
			process.write_memory(result.instruction, {&ret, 1});
			process.protect_image(page, 4096, PROT_READ | PROT_EXEC);
			const auto saved = process.context();
			const auto boundary = process.map_memory(32768, PROT_READ | PROT_WRITE | PROT_EXEC, 0) + 16384;
			const uint8_t syscall_ret[]{0x0f, 0x05, 0xc3};
			process.write_memory(boundary - 2, syscall_ret);
			process.protect_memory(boundary, 16384, PROT_NONE);
			auto crossing = saved;
			crossing.registers.rip = boundary;
			process.set_context(crossing);
			reject(boundary); // syscall bytes readable/executable, ret page inaccessible
			process.set_context(saved);
			process.enter_nt_service(result.instruction, 0x1234);
			check(process.context().registers.orig_rax == uint64_t(-1), "NT service still marked syscall");
			check(process.api_arguments(1)[0] == process.context().registers.r10, "NT argument not restored");
			process.return_api(42);
			result = process.resume();
		}
		if (mode == "selfmodify") {
			check(result.stop == raven::NativeStop::returned && result.value == 1, "first code result mismatch");
			const auto address = loaded.base() + image.entry;
			process.protect_image(address & ~uint64_t(4095), 4096, PROT_READ | PROT_WRITE | PROT_EXEC);
			const uint8_t code[]{0xb8, 42, 0, 0, 0, 0xc3};
			process.write_memory(address, code);
			process.protect_image(address & ~uint64_t(4095), 4096, PROT_READ | PROT_EXEC);
			result = process.call(address, {});
		} else if (mode == "timeout") {
			check(result.stop == raven::NativeStop::timeout, "doorbell failed to interrupt guest loop");
			auto context = process.context();
			check(context.registers.rip == loaded.base() + image.entry, "doorbell RIP imprecise");
			context.registers.rip += 2; context.registers.rax = 42;
			process.set_context(context); result = process.resume();
		} else if (matrix) {
			if (expected_fault) {
				check(result.stop == raven::NativeStop::exception && result.value == 0xc0000094, "divide fault not delivered");
				auto context = process.context();
				check(context.registers.rip == loaded.base() + image.entry + divide_offset, "divide RIP imprecise");
				check(context.registers.rax == fault_rax && context.registers.rdx == fault_rdx && context.registers.rcx == fault_rcx,
					"divide fault changed RAX/RDX/RCX before dispatch");
				check(context.registers.eflags == fault_flags, ("divide fault changed RFLAGS: got " + std::to_string(context.registers.eflags) + " expected " + std::to_string(fault_flags)).c_str());
				context.registers.rip += divide_length;
				process.set_context(context);
				result = process.resume();
			}
			check(result.stop == raven::NativeStop::syscall && result.value == 0x1235, "divide continuation failed");
			if (!expected_fault) {
				const auto context = process.context();
				check(context.registers.rbx == quotient_register && context.registers.rsi == remainder_register, "division changed valid quotient/remainder");
			}
			result = process.resume();
		} else if (mode == "ud2") {
			check(result.stop == raven::NativeStop::exception && result.value == 0xc000001d, "UD2 misclassified");
			check(process.context().registers.rip == loaded.base() + image.entry, "UD2 RIP imprecise");
			std::cout << "PASS: UD2 precise illegal instruction\n";
			return 0;
		} else if (mode == "syscall") {
			check(result.stop == raven::NativeStop::syscall && result.value == 0x1234, "syscall stop lost number");
			auto context = process.context();
			check(context.registers.rip == loaded.base() + image.entry + 7, "syscall RIP not advanced");
			context.registers.rax = 37;
			process.set_context(context);
			result = process.resume();
		} else if (mode == "fault") {
			check(result.stop == raven::NativeStop::exception && result.value == 0xc0000005 && result.fault_address == 0,
				"AV stop lost exception or address");
			check(result.exception_information == std::vector<uint64_t>{0, 0}, "read AV information count/type/address differs");
			auto context = process.context();
			check(context.registers.rip == loaded.base() + image.entry + 10, "AV RIP imprecise");
			context.registers.rip += 3;
			context.registers.rax = 37;
			process.set_context(context);
			result = process.resume();
		} else if (mode == "divide") {
			std::cerr << "divide exception code=" << std::hex << result.value << std::dec << '\n';
			check(result.stop == raven::NativeStop::exception && result.value == 0xc0000094, "divide error misclassified");
			const auto context = process.context();
			check(context.registers.rip == loaded.base() + image.entry + 15, "divide mode RIP imprecise");
			check(context.registers.rax == 1 && context.registers.rdx == 0 && context.registers.rcx == 0 &&
				context.registers.eflags == fault_flags, ("divide mode modified fault registers/flags: flags=" + std::to_string(context.registers.eflags)).c_str());
			return 0;
		} else if (mode == "breakpoint") {
			check(result.stop == raven::NativeStop::exception && result.value == 0x80000003, "provider breakpoint not delivered");
			check(process.context().registers.rip == loaded.base() + image.entry, "breakpoint RIP imprecise");
			std::cout << "PASS: INT3 precise breakpoint\n";
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
