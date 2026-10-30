// SPDX-License-Identifier: MIT
#pragma once
// Bytes comes from the bounds-checked PE mapper.
struct PEView {
	Bytes bytes;
	size_t opt;
	uint32_t directory(unsigned index) const {
		if (index >= bytes.get<uint32_t>(opt + 108))
			return 0;
		return bytes.get<uint32_t>(opt + 112 + index * 8);
	}
	size_t rva(uintptr_t pointer, size_t size = 1) const {
		auto base = reinterpret_cast<uintptr_t>(bytes.data);
		if (pointer < base)
			throw std::runtime_error("PE pointer below image");
		auto offset = pointer - base;
		bytes.check(offset, size);
		return offset;
	}
	size_t metadata() const {
		auto config = directory(10);
		if (!config || bytes.get<uint32_t>(config) < 208)
			throw std::runtime_error("missing CHPE load config");
		auto at = rva(bytes.get<uint64_t>(config + 200), 80);
		if (bytes.get<uint32_t>(at) != 1)
			throw std::runtime_error("unsupported CHPE version");
		return at;
	}
	bool ec_code(uint32_t address) const {
		auto m = metadata();
		auto map = bytes.get<uint32_t>(m + 4), count = bytes.get<uint32_t>(m + 8);
		bytes.check(map, size_t(count) * 8);
		for (uint32_t i = 0; i < count; ++i) {
			auto start = bytes.get<uint32_t>(map + i * 8),
					size = bytes.get<uint32_t>(map + i * 8 + 4);
			if ((start & 3) == 1 && address >= (start & ~3u) && address - (start & ~3u) < size)
				return true;
		}
		return false;
	}
	uint32_t native_rva(uint32_t source) const {
		if (ec_code(source))
			return source;
		auto m = metadata();
		auto table = bytes.get<uint32_t>(m + 16), count = bytes.get<uint32_t>(m + 52);
		bytes.check(table, size_t(count) * 8);
		for (uint32_t i = 0; i < count; ++i)
			if (bytes.get<uint32_t>(table + i * 8) == source) {
				auto target = bytes.get<uint32_t>(table + i * 8 + 4);
				if (!ec_code(target))
					throw std::runtime_error("CHPE redirection outside EC code");
				return target;
			}
		throw std::runtime_error("no native export redirection");
	}
	void *export_address(const char *name) const {
		auto e = directory(0);
		if (!e)
			return nullptr;
		auto n = bytes.get<uint32_t>(e + 24), names = bytes.get<uint32_t>(e + 32),
				ordinals = bytes.get<uint32_t>(e + 36), funcs = bytes.get<uint32_t>(e + 28);
		for (uint32_t i = 0; i < n; ++i)
			if (!strcmp(bytes.string(bytes.get<uint32_t>(names + i * 4)), name)) {
				auto ord = bytes.get<uint16_t>(ordinals + i * 2);
				if (ord >= bytes.get<uint32_t>(e + 20))
					throw std::runtime_error("invalid export ordinal");
				auto source = bytes.get<uint32_t>(funcs + ord * 4);
				return bytes.data + native_rva(source);
			}
		return nullptr;
	}
	void install_tls(RavenNT::ThreadEnvironment &thread) {
		auto tls = directory(9);
		if (!tls)
			return;
		auto start = bytes.get<uint64_t>(tls), end = bytes.get<uint64_t>(tls + 8);
		if (end < start)
			throw std::runtime_error("invalid TLS template");
		auto offset = rva(start, end - start);
		bytes.put<uint32_t>(rva(bytes.get<uint64_t>(tls + 16), 4), 0);
		thread.install_tls(bytes.data + offset, end - start, bytes.get<uint32_t>(tls + 32));
	}
};
