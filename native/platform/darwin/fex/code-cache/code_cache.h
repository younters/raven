// SPDX-License-Identifier: MIT
#pragma once
// Opt-in cross-process translation cache for the x64 FEX module (see DESIGN.md).
// Included by fex_process.cpp after the embedded module host.
#include <CommonCrypto/CommonDigest.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>

namespace raven_code_cache {
namespace fs = std::filesystem;

struct Config {
	bool enabled = false;
	fs::path directory;
	uint64_t cap = 2048ull << 20;
	// Host-side milliseconds: namespace preparation, module start-up (index load), image registration.
	double prepare_ms = 0, start_ms = 0, register_ms = 0;
	std::chrono::steady_clock::time_point mark;
};
inline double since(std::chrono::steady_clock::time_point start)
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}
inline Config config;

inline std::string module_id(const std::string &path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file) return {};
	CC_SHA256_CTX context;
	CC_SHA256_Init(&context);
	std::vector<char> buffer(1 << 20);
	while (file) {
		file.read(buffer.data(), buffer.size());
		CC_SHA256_Update(&context, buffer.data(), CC_LONG(file.gcount()));
	}
	unsigned char digest[CC_SHA256_DIGEST_LENGTH];
	CC_SHA256_Final(digest, &context);
	char hex[17];
	for (int i = 0; i < 8; ++i) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
	return hex;
}

// Newest modification time and total size of the regular files below a directory.
inline std::pair<fs::file_time_type, uint64_t> survey(const fs::path &directory)
{
	std::error_code error;
	fs::file_time_type newest = fs::last_write_time(directory, error);
	uint64_t size = 0;
	for (fs::recursive_directory_iterator it(directory, fs::directory_options::skip_permission_denied, error), end; !error && it != end; it.increment(error)) {
		if (!it->is_regular_file(error)) continue;
		size += it->file_size(error);
		newest = std::max(newest, it->last_write_time(error));
	}
	return {newest, size};
}

// Rename aside first: processes still holding the old files keep their descriptors,
// and a concurrent evictor loses the rename race instead of deleting new files.
inline void discard(const fs::path &directory, const char *reason)
{
	std::error_code error;
	auto aside = directory;
	aside += std::string(".") + reason + "-" + std::to_string(getpid());
	fs::rename(directory, aside, error);
	if (!error) fs::remove_all(aside, error);
}

// The main file stops growing at this size, leaving room for its index under the cap.
inline uint64_t main_file_cap(uint64_t cap)
{
	return cap - cap / 32;
}

// Evicts a full namespace (so the next runs refill it rather than never caching new code),
// namespaces of other module builds unused for 14 days, and leftovers of interrupted discards.
inline void evict(const fs::path &base, const fs::path &current, uint64_t cap)
{
	std::error_code error;
	if (fs::exists(current, error) && survey(current).second >= main_file_cap(cap)) discard(current, "evict");
	const auto now = fs::file_time_type::clock::now();
	for (fs::directory_iterator it(base, error), end; !error && it != end; it.increment(error)) {
		if (!it->is_directory(error) || it->path() == current) continue;
		const auto name = it->path().filename().string();
		const bool leftover = name.find('.') != std::string::npos;
		const auto age = now - survey(it->path()).first;
		if (age > (leftover ? std::chrono::hours(1) : std::chrono::hours(24 * 14))) {
			if (leftover) fs::remove_all(it->path(), error);
			else discard(it->path(), "stale");
		}
	}
}

// Called before the module initializes; selects the namespace for this module build.
inline void prepare(const std::string &module_path)
{
	const char *flag = std::getenv("RAVEN_FEX_CODE_CACHE");
	if (!flag || strcmp(flag, "1")) return;
	const auto begin = std::chrono::steady_clock::now();
	const auto id = module_id(module_path);
	if (id.empty()) return;
	fs::path base;
	if (const char *dir = std::getenv("RAVEN_FEX_CODE_CACHE_DIR"); dir && *dir) base = dir;
	else if (const char *home = std::getenv("HOME"); home && *home) base = fs::path(home) / "Library/Caches/Raven/fex-code";
	else return;
	if (const char *mb = std::getenv("RAVEN_FEX_CODE_CACHE_MAX_MB")) {
		const auto value = strtoull(mb, nullptr, 10);
		if (value >= 1 && value <= 65536) config.cap = value << 20;
	}
	std::error_code error;
	fs::create_directories(base, error);
	if (error) return;
	config.directory = base / id;
	evict(base, config.directory, config.cap);
	fs::create_directories(config.directory, error);
	config.enabled = !error;
	config.prepare_ms = since(begin);
	config.mark = std::chrono::steady_clock::now();
}

// UTF-8 to UTF-16, so a cache directory under a non-ASCII home folder reaches FEX intact.
inline std::u16string widen(const std::string &text)
{
	std::u16string wide;
	for (size_t i = 0; i < text.size();) {
		const unsigned char lead = text[i];
		const size_t length = lead < 0x80 ? 1 : lead >> 5 == 6 ? 2 : lead >> 4 == 14 ? 3 : lead >> 3 == 30 ? 4 : 0;
		if (!length || i + length > text.size()) {
			wide += u'\ufffd';
			++i;
			continue;
		}
		char32_t code = length == 1 ? lead : lead & (0x7f >> length);
		for (size_t k = 1; k < length; ++k)
			code = code << 6 | (text[i + k] & 0x3f);
		i += length;
		if (code >= 0x10000) {
			code -= 0x10000;
			wide += char16_t(0xd800 + (code >> 10));
			wide += char16_t(0xdc00 + (code & 0x3ff));
		} else
			wide += char16_t(code);
	}
	return wide;
}

// FEX's own DiskCache options, read by the module's config loader at ProcessInit.
inline void configure(RavenNT::ProcessEnvironment &environment)
{
	if (!config.enabled) return;
	const auto path = config.directory.string() + "/";
	for (const std::string &entry : {std::string("FEX_DISKCACHE=1"), "FEX_DISKCACHEPATH=" + path, std::string("FEX_DISKCACHEANONCACHING=0"),
		     std::string("FEX_DISKCACHEFILEMAPPING=0"), std::string("FEX_DISKCACHEMEMORYSIZE=0"),
		     std::string("FEX_DISKCACHEVALIDATION=0"), "FEX_DISKCACHEMAXFILESIZE=" + std::to_string(main_file_cap(config.cap))})
		environment.add_environment(widen(entry));
}

// Values: hits, misses, stores, rejected, skipped, damaged, reading, flush timed out. Op 3 flushes first.
inline bool stats(uint64_t op, uint64_t (&values)[8], void *teb)
{
	uint64_t args[8]{op, uintptr_t(values)};
	return raven_ec_call(mapped_image->export_address("RavenCodeCache"), args, teb) != 0;
}

// FEX refuses a database whose Fossilize header is wrong, and would keep refusing it.
inline bool unreadable_header(const fs::path &directory)
{
	static const unsigned char magic[15] = {0x81, 'F', 'O', 'S', 'S', 'I', 'L', 'I', 'Z', 'E', 'D', 'B', 0, 0, 0};
	std::error_code error;
	for (fs::recursive_directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
		if (it->path().extension() != ".foz" || !it->is_regular_file(error)) continue;
		unsigned char header[15]{};
		std::ifstream file(it->path(), std::ios::binary);
		if (file.read(reinterpret_cast<char *>(header), sizeof(header)) && memcmp(header, magic, sizeof(magic))) return true;
	}
	return false;
}

// After ProcessInit: keep the cache only if FEX opened it; set aside a damaged database.
inline void started(void *teb)
{
	if (!config.enabled) return;
	config.start_ms = since(config.mark);
	uint64_t values[8]{};
	if (!mapped_image->export_address("RavenCodeCache") || !stats(2, values, teb)) {
		config.enabled = false;
		if (unreadable_header(config.directory)) discard(config.directory, "damaged");
		return;
	}
	if (values[5]) discard(config.directory, "damaged");
}

inline void register_image(uint64_t address, uint64_t size, void *teb)
{
	if (!config.enabled) return;
	const auto begin = std::chrono::steady_clock::now();
	uint64_t args[8]{1, address, size};
	raven_ec_call(mapped_image->export_address("RavenCodeCache"), args, teb);
	config.register_ms += since(begin);
}

inline void finished(void *teb)
{
	if (!config.enabled) return;
	uint64_t values[8]{};
	const auto begin = std::chrono::steady_clock::now();
	if (!stats(3, values, teb) || !std::getenv("RAVEN_FEX_CODE_CACHE_STATS")) return;
	fprintf(stderr, "FEX code cache pid=%d hits=%llu misses=%llu stores=%llu rejected=%llu skipped=%llu damaged=%llu "
		"flush_timeout=%llu prepare_ms=%.1f start_ms=%.1f register_ms=%.1f flush_ms=%.1f\n", getpid(),
		(unsigned long long)values[0], (unsigned long long)values[1], (unsigned long long)values[2],
		(unsigned long long)values[3], (unsigned long long)values[4], (unsigned long long)values[5], (unsigned long long)values[7],
		config.prepare_ms, config.start_ms, config.register_ms, since(begin));
}
} // namespace raven_code_cache
