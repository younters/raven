// SPDX-License-Identifier: MIT
#pragma once
#include "nt_strings.h"
#include <array>
#include <atomic>
#include <pthread.h>
#include <sys/mman.h>
namespace RavenNT {
struct CpuArea {
	uint8_t simulation, syscall;
	uint8_t pad[6];
	uint64_t stack_base, stack_limit;
	void *context;
	uint32_t *doorbell;
	uint64_t loading;
	uint64_t emulator[4], inline_data;
};
static_assert(sizeof(CpuArea) == 0x58 && offsetof(CpuArea, emulator) == 0x30);
struct ProcessEnvironment {
	alignas(16) std::array<uint8_t, 0x800> peb{};
	alignas(16) std::array<uint8_t, 0x800> parameters{};
	std::u16string environment = u"LOCALAPPDATA=/tmp/raven-fex";
	std::u16string image_path = u"/raven-fex-modhost";
	uint32_t tls_bits[2]{1, 0};
	Bitmap tls_bitmap{64, 0, tls_bits};
	void *code_bitmap;
	static constexpr size_t bitmap_size = 1ull << 32; // one bit per 4K page in 47-bit user VA
	template <class T> void put(size_t at, T v) {
		memcpy(peb.data() + at, &v, sizeof(v));
	}
	ProcessEnvironment() {
		environment.push_back(0);
		environment.push_back(0);
		code_bitmap =
			mmap(nullptr, bitmap_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (code_bitmap == MAP_FAILED)
			throw std::runtime_error("EC code bitmap reservation failed");
		put(0x20, parameters.data());
		put(0x30, this);
		put(0x78, &tls_bitmap);
		put<uint32_t>(0xb8, sysconf(_SC_NPROCESSORS_ONLN));
		put(0x368, code_bitmap);
		String path{};
		init_unicode(&path, image_path.c_str());
		memcpy(parameters.data() + 0x60, &path, sizeof(path));
		memcpy(parameters.data() + 0x70, &path, sizeof(path));
		auto *env = environment.data();
		memcpy(parameters.data() + 0x80, &env, 8);
	}
	~ProcessEnvironment() {
		munmap(code_bitmap, bitmap_size);
	}
	// NAME=value before the terminating empty entry; ProcessInit reads it through the CRT.
	void add_environment(std::u16string_view entry) {
		environment.insert(environment.size() - 1, entry);
		environment.insert(environment.size() - 1, 1, u'\0');
		auto *env = environment.data();
		memcpy(parameters.data() + 0x80, &env, 8);
	}
	void mark_ec(uintptr_t begin, size_t size, bool present = true) {
		if (begin >= (1ull << 47) || size > (1ull << 47) - begin)
			throw std::runtime_error("EC address outside bitmap");
		for (auto p = begin >> 12; p < (begin + size + 4095) >> 12; ++p) {
			auto &byte = static_cast<uint8_t *>(code_bitmap)[p / 8];
			if (present)
				__atomic_fetch_or(&byte, uint8_t(1u << (p % 8)), __ATOMIC_RELEASE);
			else
				__atomic_fetch_and(&byte, uint8_t(~(1u << (p % 8))), __ATOMIC_RELEASE);
		}
	}
};
struct ThreadEnvironment {
	inline static std::atomic<unsigned> live_environments{0};
	~ThreadEnvironment() {
		--live_environments;
	}
	alignas(16) std::array<uint8_t, 0x3000> teb{};
	alignas(16) std::array<uint8_t, 0x600> context{};
	std::array<void *, 64> tls_vector{};
	std::vector<uint8_t> static_tls;
	CpuArea cpu{};
	template <class T> void put(size_t at, T v) {
		memcpy(teb.data() + at, &v, sizeof(v));
	}
	explicit ThreadEnvironment(ProcessEnvironment &process) {
		uint64_t tid = 0;
		pthread_threadid_np(nullptr, &tid);
		auto high = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pthread_self()));
		put(8, high);
		put(16, high - pthread_get_stacksize_np(pthread_self()));
		put(0x30, teb.data());
		put<uint64_t>(0x40, getpid());
		put(0x48, tid);
		put(0x58, tls_vector.data());
		put(0x60, process.peb.data());
		put(0x1788, &cpu);
		cpu.context = context.data();
		++live_environments;
	}
	void install_tls(const void *data, size_t initialized, size_t zero_fill) {
		if (initialized > SIZE_MAX - zero_fill)
			throw std::runtime_error("TLS size overflow");
		static_tls.resize(initialized + zero_fill + 16);
		if (initialized)
			memcpy(static_tls.data(), data, initialized);
		tls_vector[0] = static_tls.data();
	}
};
} // namespace RavenNT
