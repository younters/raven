#include "commit.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <fstream>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>

namespace raven {

HostMemory host_memory()
{
	struct sysinfo info{};
	sysinfo(&info);
	const uint64_t unit = info.mem_unit ? info.mem_unit : 1;
	const uint64_t total = info.totalram * unit;
	return {total, std::min(total, (info.freeram + info.bufferram) * unit),
		(info.totalswap - std::min(info.totalswap, info.freeswap)) * unit};
}

size_t system_commit_limit()
{
	static const size_t limit = [] {
		const auto ram = host_memory().total;
		struct statvfs volume{};
#ifdef __APPLE__
		const char *path = "/private/var/vm";
#else
		const char *path = "/";
#endif
		const uint64_t model = std::max(3 * ram, uint64_t(4) << 30);
		const uint64_t pagefile = statvfs(path, &volume) == 0 ?
			std::min(model, uint64_t(volume.f_blocks) * volume.f_frsize / 8) : model;
		return size_t(ram + pagefile);
	}();
	return limit;
}

size_t process_commit_limit()
{
	if (const char *value = std::getenv("RAVEN_COMMIT_LIMIT_MB")) {
		char *end = nullptr;
		const auto mb = std::strtoull(value, &end, 10);
		if (*value != '-' && end != value && !*end && mb <= std::numeric_limits<size_t>::max() / (1024*1024))
			return size_t(mb) * 1024*1024;
	}
	return system_commit_limit();
}

static std::atomic<uint64_t> peak{0}, others{0};
static std::atomic<bool> sampled{false};

ProcessResident process_resident()
{
	uint64_t pages = 0, resident = 0, shared = 0;
#ifdef __APPLE__
	auto statm = raven_darwin::proc_stream("/proc/self/statm");
#else
	std::ifstream statm("/proc/self/statm");
#endif
	statm >> pages >> resident >> shared;
	// Darwin's proc shim reports shared=0: private resident is conservatively approximated by RSS there.
	return {resident * 4096, (resident - std::min(resident, shared)) * 4096};
}

void refresh_commit_sample()
{
	const auto host = host_memory();
	const auto resident = process_resident().private_bytes;
	const auto host_used = host.total - host.available;
	others.store(host_used - std::min(host_used, resident) + host.swap_used);
	sampled.store(true);
}

uint64_t system_commit_charge(uint64_t own)
{
	if (!sampled.load()) refresh_commit_sample();
	return others.load() + own;
}

void note_commit(uint64_t own, uint64_t own_peak)
{
	const auto charge = system_commit_charge(std::max(own, own_peak));
	auto old = peak.load();
	while (old < charge && !peak.compare_exchange_weak(old, charge)) {}
}

uint64_t system_commit_peak(uint64_t own)
{
	note_commit(own, own);
	return peak.load();
}

}
