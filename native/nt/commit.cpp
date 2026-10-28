#include "commit.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <sys/sysinfo.h>
#include <sys/statvfs.h>
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
        if (const char *value = std::getenv("RAVEN_COMMIT_LIMIT_MB")) {
            char *end = nullptr;
            const auto mb = std::strtoull(value, &end, 10);
            if (end != value && !*end && mb <= std::numeric_limits<size_t>::max() / (1024*1024))
                return size_t(mb) * 1024*1024;
        }
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
static std::atomic<uint64_t> peak{0};
uint64_t system_commit_charge(uint64_t own)
{
    const auto host = host_memory();
    const auto charge = own + host.total - host.available + host.swap_used;
    auto old = peak.load();
    while (old < charge && !peak.compare_exchange_weak(old, charge)) {}
    return charge;
}
uint64_t system_commit_peak(uint64_t own)
{
    system_commit_charge(own);
    return peak.load();
}
}
