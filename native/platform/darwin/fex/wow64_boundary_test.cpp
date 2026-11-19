// SPDX-License-Identifier: MIT
#define RAVEN_FEX_EMBEDDED
#include "modhost/native_host.cpp"
#include "guest_window.h"
#include <thread>
#include <atomic>


static bool run_probes = false, run_reserved = false;
static void verify_private_teb()
{
	auto *saved = RavenNT::active_thread;
	RavenNT::active_thread = nullptr;
	auto *first = static_cast<uint8_t *>(private_teb());
	first[0x1480] = 42;
	std::atomic<bool> ready{false}, done{false};
	void *other = nullptr;
	std::thread worker([&] {
		other = private_teb();
		assert(static_cast<uint8_t *>(other)[0x1480] == 0);
		static_cast<uint8_t *>(other)[0x1480] = 99;
		ready.store(true);
		while (!done.load()) std::this_thread::yield();
		assert(private_teb() == other);
	});
	while (!ready.load()) std::this_thread::yield();
	assert(other != first && first[0x1480] == 42 && private_teb() == first);
	done.store(true);
	worker.join();
	RavenNT::active_thread = saved;
	assert(private_teb() == saved->teb.data());
}
static void verify_module_probes()
{
	RavenFault::Scope faults(mapped_image->export_address("RavenHandleFault"));
	void *page = nullptr;
	size_t size = 65536;
	assert(!RavenVM::allocate(reinterpret_cast<void *>(-1), &page, 0, &size, 0x2000, 1));
	uint64_t args[8]{uintptr_t(page), size, 0x123456789abcdef0ULL};
	assert(raven_ec_call(mapped_image->export_address("RavenTestOvercommit"), args,
		thread_environment->teb.data()) == args[2]);
	assert(*static_cast<uint64_t *>(page) == args[2]);
	// A synthetic Mach-O PC is rejected by the same module fault entry.

	RavenFault::Record record{};
	record.code = 0xc0000005; record.count = 2; record.information[1] = uintptr_t(page) + 32768;
	RavenFault::NativeContext context{};
	context.pc = uintptr_t(&verify_module_probes);
	uint64_t fault_args[8]{uintptr_t(&record), uintptr_t(&context)};
	assert(uint32_t(raven_ec_call(mapped_image->export_address("RavenHandleFault"), fault_args,
		thread_environment->teb.data())) == 0xc00000bb);
	// Keep A alive while B frees A's allocations. Reuse heaps over many rounds;
	// change the execution TEB on each entry while keeping private identity fixed.
	std::atomic<unsigned> phase{0};
	void *pointers[256]{};
	std::thread worker([&] {
		RavenNT::ThreadEnvironment env(*process_environment);
		RavenNT::active_thread = &env;
		mapped_image->install_tls(env);
		uint64_t words[8]{};
		assert(!raven_ec_call(mapped_image->export_address("ThreadInit"), words, env.teb.data()));
		for (unsigned round = 0; round < 64; ++round) {
			while (phase.load() != 1) std::this_thread::yield();
			for (void *pointer : pointers) {
				words[0] = uintptr_t(pointer);
				raven_ec_call(mapped_image->export_address("RavenTestFree"), words, env.teb.data());
			}
			phase.store(2);
		}
		words[0] = uint64_t(-2);
		assert(!raven_ec_call(mapped_image->export_address("ThreadTerm"), words, env.teb.data()));
		RavenNT::active_thread = nullptr;
	});
	alignas(16) std::array<uint8_t, 0x3000> guest[2]{};
	for (auto &teb : guest) {
		memcpy(teb.data(), thread_environment->teb.data(), teb.size());
		auto *self = teb.data();
		memcpy(teb.data() + 0x30, &self, sizeof(self));
	}
	for (unsigned round = 0; round < 64; ++round) {
		for (auto &pointer : pointers) {
			args[0] = 128 + round * 16;
			pointer = reinterpret_cast<void *>(raven_ec_call(mapped_image->export_address("RavenTestAllocate"), args,
				guest[round % 2].data()));
			assert(pointer);
		}
		phase.store(1);
		while (phase.load() != 2) std::this_thread::yield();
		phase.store(0);
	}
	worker.join();
	std::cout << "PASS: module overcommit write and cross-thread rpmalloc handoff\n";
}
int main(int argc, char **argv)
{
	try {
		if (argc != 3) return 2;
		wow64_module = std::string_view(argv[2]) == "wow64";
		run_probes = std::string_view(argv[2]) == "x64-probes";
		run_reserved = std::string_view(argv[2]) == "x64-reserved";
		std::unique_ptr<raven::FexGuestWindow> window;
		if (wow64_module) { window = std::make_unique<raven::FexGuestWindow>(); wow64_guest_base = window->base(); }
		char *args[]{argv[0], argv[1]};
		return map_module(2, args, resolve, inspect_image, +[](Bytes bytes, size_t opt) {
			initialize_image(bytes, opt);
			verify_host_callret_fault();
			if (run_reserved) {
				RavenFault::Scope faults(mapped_image->export_address("RavenHandleFault"));
				void *reserved = nullptr;
				size_t size = 16384;
				assert(!RavenVM::allocate(reinterpret_cast<void *>(-1), &reserved, 0, &size, 0x2000, 1));
				*static_cast<volatile uint64_t *>(reserved) = 42;
				throw std::runtime_error("non-module reserved write unexpectedly succeeded");
			}
			verify_private_teb();
			if (run_probes) verify_module_probes();
		});
	} catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
