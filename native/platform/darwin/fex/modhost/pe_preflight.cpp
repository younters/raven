// SPDX-License-Identifier: MIT
// Standalone mode inspects PE data. Host mode can bind imports and invoke a
// gated executor only after every import resolves.
#include <sys/mman.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

struct Bytes {
	uint8_t *data;
	size_t size;
	void check(size_t at, size_t count) const {
		if (at > size || count > size - at)
			throw std::runtime_error("PE range outside buffer");
	}
	template <class T> T get(size_t at) const {
		check(at, sizeof(T));
		T v;
		std::memcpy(&v, data + at, sizeof(v));
		return v;
	}
	template <class T> void put(size_t at, T v) {
		check(at, sizeof(T));
		std::memcpy(data + at, &v, sizeof(v));
	}
	const char *string(size_t at) const {
		check(at, 1);
		if (!std::memchr(data + at, 0, size - at))
			throw std::runtime_error("unterminated PE string");
		return reinterpret_cast<const char *>(data + at);
	}
};
struct Mapping {
	void *base;
	size_t size;
	~Mapping() {
		if (base != MAP_FAILED)
			munmap(base, size);
	}
};
#ifdef RAVEN_HOST_LOADER
using ImportResolver = void *(*)(const char *, const char *);
using MapObserver = void (*)(Bytes, size_t);
int map_module(int argc, char **argv, ImportResolver resolve, MapObserver observer = nullptr,
	MapObserver execute = nullptr, Mapping *retained = nullptr) {
#else
int main(int argc, char **argv) {
#endif
	if (argc != 2) {
		std::cerr << "usage: pe-preflight module.dll\n";
		return 2;
	}
	try {
		std::ifstream input(argv[1], std::ios::binary);
		if (!input)
			throw std::runtime_error("cannot open input");
		std::vector<uint8_t> file((std::istreambuf_iterator<char>(input)), {});
		Bytes f{file.data(), file.size()};
		if (f.get<uint16_t>(0) != 0x5a4d)
			throw std::runtime_error("not MZ");
		size_t pe = f.get<uint32_t>(0x3c), opt = pe + 24;
		if (f.get<uint32_t>(pe) != 0x4550 || f.get<uint16_t>(opt) != 0x20b)
			throw std::runtime_error("not PE32+");
		auto machine = f.get<uint16_t>(pe + 4);
		if (machine != 0xaa64 && machine != 0x8664)
			throw std::runtime_error("unsupported machine");
		auto nsec = f.get<uint16_t>(pe + 6), optsize = f.get<uint16_t>(pe + 20);
		if (optsize < 160)
			throw std::runtime_error("short optional header");
		auto image_size = f.get<uint32_t>(opt + 56), headers = f.get<uint32_t>(opt + 60);
		if (!image_size || image_size > 256 * 1024 * 1024)
			throw std::runtime_error("invalid image size");
		Mapping mapping{
			mmap(nullptr, image_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0),
			image_size};
		if (mapping.base == MAP_FAILED)
			throw std::runtime_error("mmap failed");
		Bytes image{static_cast<uint8_t *>(mapping.base), image_size};
		f.check(0, headers);
		image.check(0, headers);
		std::memcpy(image.data, f.data, headers);
		for (size_t i = 0; i < nsec; ++i) {
			size_t s = opt + optsize + i * 40;
			f.check(s, 40);
			auto va = f.get<uint32_t>(s + 12), raw = f.get<uint32_t>(s + 16),
					offset = f.get<uint32_t>(s + 20);
			image.check(va, f.get<uint32_t>(s + 8));
			image.check(va, raw);
			f.check(offset, raw);
			std::memcpy(image.data + va, f.data + offset, raw);
		}
		if (f.get<uint32_t>(opt + 108) < 6)
			throw std::runtime_error("missing PE directories");
		uint64_t delta = reinterpret_cast<uintptr_t>(mapping.base) - f.get<uint64_t>(opt + 24);
		auto reloc = f.get<uint32_t>(opt + 112 + 5 * 8), rsize = f.get<uint32_t>(opt + 116 + 5 * 8);
		image.check(reloc, rsize);
		size_t fixed = 0;
		for (size_t at = reloc, end = size_t(reloc) + rsize; at < end;) {
			if (end - at < 8)
				throw std::runtime_error("short relocation block");
			auto page = image.get<uint32_t>(at), len = image.get<uint32_t>(at + 4);
			if (len < 8 || len > end - at || len % 2)
				throw std::runtime_error("bad relocation block");
			for (size_t e = at + 8; e < at + len; e += 2) {
				auto fix = image.get<uint16_t>(e);
				auto type = fix >> 12;
				if (!type)
					continue;
				if (type != 10)
					throw std::runtime_error("non-DIR64 relocation requires implementation");
				size_t target = size_t(page) + (fix & 4095);
				image.put<uint64_t>(target, image.get<uint64_t>(target) + delta);
				++fixed;
			}
			at += len;
		}
		std::cout << "machine=0x" << std::hex << machine << std::dec << " sections=" << nsec
					<< " image_size=" << image_size << " DIR64_relocations=" << fixed << '\n';
#ifdef RAVEN_HOST_LOADER
		if (observer)
			observer(image, opt);
#endif
		auto imports = f.get<uint32_t>(opt + 120), isize = f.get<uint32_t>(opt + 124);
		image.check(imports, isize);
		size_t count = 0, resolved = 0, auxiliary_writes = 0;
		for (size_t at = imports, end = size_t(imports) + isize; at + 20 <= end; at += 20) {
			auto name = image.get<uint32_t>(at + 12);
			if (!name)
				break;
			auto lookup = image.get<uint32_t>(at), iat = image.get<uint32_t>(at + 16);
			if (!lookup)
				lookup = iat;
			for (size_t i = 0;; ++i) {
				auto thunk = image.get<uint64_t>(lookup + i * 8);
				if (!thunk)
					break;
				image.check(iat + i * 8, 8);
#ifdef RAVEN_HOST_LOADER
				if (!(thunk >> 63)) {
					if (void *target = resolve(image.string(name), image.string(thunk + 2))) {
						image.put<uint64_t>(iat + i * 8, reinterpret_cast<uintptr_t>(target));
						// The EC frontend loads its native imports through the auxiliary IAT.
						// Its layout mirrors the ordinary IAT, including descriptor terminators.
						auto load_config = image.get<uint32_t>(opt + 112 + 10 * 8);
						if (load_config) {
							auto metadata_va = image.get<uint64_t>(load_config + 200);
							if (metadata_va) {
								auto metadata =
									metadata_va - reinterpret_cast<uintptr_t>(image.data);
								auto auxiliary = image.get<uint32_t>(metadata + 44);
								auto iat_base = image.get<uint32_t>(opt + 112 + 12 * 8);
								if (auxiliary) {
									auto slot = auxiliary + iat + i * 8 - iat_base;
									image.put<uint64_t>(slot, reinterpret_cast<uintptr_t>(target));
									if (image.get<uint64_t>(slot) !=
										reinterpret_cast<uintptr_t>(target))
										throw std::runtime_error(
											"auxiliary IAT write verification failed");
									++auxiliary_writes;
								}
							}
						}
						++resolved;
						continue;
					}
				}
#endif
				std::cout << "unresolved " << image.string(name) << '!';
				if (thunk >> 63)
					std::cout << '#' << (thunk & 65535);
				else
					std::cout << image.string(thunk + 2);
				std::cout << '\n';
				++count;
			}
		}
		std::cout << "auxiliary_iat_writes=" << auxiliary_writes << "\n";
		std::cout << "resolved=" << resolved << "\n";
#ifdef RAVEN_HOST_LOADER
		if (!count && execute) {
			if (machine == 0x8664 && (resolved != 127 || auxiliary_writes != 127))
				throw std::runtime_error("expected 127 ordinary and auxiliary IAT writes");
			if (machine == 0xaa64 && (resolved != 128 || auxiliary_writes != 0))
				throw std::runtime_error("expected 128 native ARM64 imports without auxiliary IAT");
			execute(image, opt);
			if (retained) {
				*retained = mapping;
				mapping.base = MAP_FAILED;
			}
			return 0;
		}
#endif
		std::cout << "STOP: " << count
					<< " unresolved imports; inspection only, no module entry attempted\n";
		return 3;
	} catch (const std::exception &e) {
		std::cerr << e.what() << '\n';
		return 2;
	}
}
