#include "commit.h"
#include "child_process.h"
#include "kernel_context.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <unistd.h>
#include <sys/sysinfo.h>
#ifdef __APPLE__
#include "platform/darwin/include/raven_darwin_compat.h"
#endif

namespace raven {
void commit_global_status_ex(ApiCall &call, NativeMemory &memory)
{
		std::vector<uint8_t> bytes(64);
		const auto host = host_memory();
		const auto [total, available] = memory.address_space();
		const uint64_t limit = memory.commit_limit(), charge = system_commit_charge(memory.committed());
		const uint32_t length = 64, load = host.total ? 100 - host.available * 100 / host.total : 0;
		std::memcpy(bytes.data(), &length, 4);
		std::memcpy(bytes.data() + 4, &load, 4);
		const uint64_t values[] = {host.total, host.available, limit, limit - std::min<uint64_t>(limit, charge), total, available, 0};
		std::memcpy(bytes.data() + 8, values, 56);
		call.write(call[0], bytes);
		call.ret(1);
}
void commit_global_status(ApiCall &call, NativeMemory &memory)
{
		const size_t width = call.pointer_width();
		const uint64_t cap = width == 4 ? 0xffffffffULL : UINT64_MAX;
		const auto host = host_memory();
		const auto [total, available] = memory.address_space();
		const uint64_t limit = memory.commit_limit(), charge = system_commit_charge(memory.committed());
		const uint64_t values[] = {host.total, host.available, limit, limit - std::min<uint64_t>(limit, charge), total, available};
		std::vector<uint8_t> bytes(8 + 6 * width);
		const uint32_t length = uint32_t(bytes.size()), load = host.total ? 100 - host.available * 100 / host.total : 0;
		std::memcpy(bytes.data(), &length, 4);
		std::memcpy(bytes.data() + 4, &load, 4);
		for (size_t index = 0; index < 6; ++index) {
			const uint64_t value = std::min(values[index], cap);
			std::memcpy(bytes.data() + 8 + index * width, &value, width);
		}
		call.write(call[0], bytes);
}
void commit_performance_info(ApiCall &call, KernelContext &context)
{
			const size_t width = call.pointer_width(), size = width == 8 ? 104 : 56;
			if (!call[0] || call.u32(1) < size) {
				call.fail(24, 0);
				return;
			}
			struct sysinfo info {};
			sysinfo(&info);
			const uint64_t unit = info.mem_unit ? info.mem_unit : 1, page = 4096;
			const auto host = host_memory();
			const uint64_t total = host.total / page, available = host.available / page;
			const auto own = context.memory.committed();
			const uint64_t commit = system_commit_charge(own) / page, limit = context.memory.commit_limit() / page;
			const uint64_t peak = system_commit_peak(own) / page;
			std::vector<uint8_t> bytes(size, 0);
			size_t at = 0;
			const auto put = [&](uint64_t value, size_t field) {
				std::memcpy(bytes.data() + at, &value, field);
				at += field;
			};
			put(size, 4);
			at = width == 8 ? 8 : 4;
			for (const uint64_t value : {commit, limit, peak, total, available, info.bufferram * unit / page, uint64_t(0), uint64_t(0), uint64_t(0), page})
				put(value, width);
			put(256, 4);
			put(info.procs, 4);
			put(info.procs * 4u, 4);
			call.write(call[0], bytes);
			call.ret(1);
}
void commit_process_memory_info(ApiCall &call, KernelContext &context)
{
		if (!(call[0]==UINT64_MAX || call[0]==UINT32_MAX || context.objects.object_as<SelfProcessObject>(call[0]) != nullptr) || !call[1]) {
			call.fail(call[1] ? 6 : 87);
			return;
		}
		uint64_t pages = 0, resident = 0;
#ifdef __APPLE__
		auto statm = raven_darwin::proc_stream("/proc/self/statm");
#else
		std::ifstream statm("/proc/self/statm");
#endif
		statm >> pages >> resident;
		const size_t width = call.pointer_width(), size = width == 4 ? 40 : 72;
		std::vector<uint8_t> counters(call.u32(2) >= size + width ? size + width : std::min<size_t>(call.u32(2), size));
		if (counters.size() < size) {
			call.fail(24);
			return;
		}
		const uint32_t cb = uint32_t(counters.size());
		std::memcpy(counters.data(), &cb, 4);
		// Working set remains the host controller resident-size approximation.
		const uint64_t working = resident * 4096, committed = context.memory.committed();
		const auto put = [&](size_t index, uint64_t value) { std::memcpy(counters.data() + 8 + (width == 4 ? 0 : 0) + index * width, &value, width); };
		put(0, working);
		put(1, working);
		put(6, committed);
		put(7, context.memory.peak_committed());
		if (counters.size() > size) put(8, committed);
		call.write(call[1], counters);
		call.ret(1);
}
uint32_t commit_system_information(NativeProcess &process, NativeMemory &memory, std::span<const uint64_t> args)
{
    uint32_t status=0;
    const auto put=[](auto &bytes, size_t at, uint64_t value, size_t width) {
        for (size_t i=0;i<width;++i) bytes[at+i]=uint8_t(value>>(i*8));
    };
			// SYSTEM_PERFORMANCE_INFORMATION has the same ULONG commit fields at both widths.
			std::array<uint8_t,312> info{};
			const auto own=memory.committed();
			put(info,44,host_memory().available/4096,4);
			put(info,48,system_commit_charge(own)/4096,4);
			put(info,52,memory.commit_limit()/4096,4);
			put(info,56,system_commit_peak(own)/4096,4);
			if (args[3]) {
				std::array<uint8_t,4> length{}; put(length,0,info.size(),4); process.write_memory(args[3],length);
			}
			if (args[2]<info.size()) status=0xc0000004U;
			else process.write_memory(args[1],info);
    return status;
}

}
