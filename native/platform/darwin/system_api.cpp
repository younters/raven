#include <raven_darwin_system.h>
#include "nt/api.h"
#include "processor_count.h"
#include "nt/registry.h"
#include "nt/text.h"

#include <cpuid.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>
#include <thread>

namespace raven_darwin {

void register_firmware_api(raven::ApiRegistry &registry)
{
	// Windows' 'RSMB' provider exposes a RawSMBIOSData header followed by the same DMI records used for installed-memory queries.
	constexpr uint32_t rsmb = 0x52534d42;
	registry.add("EnumSystemFirmwareTables", 3, [](raven::ApiCall &call) {
		if (call.u32(0) != rsmb) { call.fail(1); return; }
		if (call[1] && call.u32(2) >= 4)
			call.store<uint32_t>(call[1], 0);
		call.ret(4);
	});
	registry.add("GetSystemFirmwareTable", 4, [](raven::ApiCall &call) {
		if (call.u32(0) != rsmb || call.u32(1)) { call.fail(1); return; }
		const auto table = smbios_table();
		std::vector<uint8_t> data{0, 3, 2, 0, 0, 0, 0, 0};
		const uint32_t length = uint32_t(table.size());
		std::memcpy(data.data() + 4, &length, sizeof length);
		data.insert(data.end(), table.begin(), table.end());
		if (call[2] && call.u32(3) >= data.size())
			call.write(call[2], data);
		call.ret(data.size());
	});
}

// The processor set query follows the guest group model.
void register_processor_count_api(raven::ApiRegistry &registry)
{
	registry.add("GetSystemCpuSetInformation",5,[](raven::ApiCall &call) {
		if (!call[2]) { call.fail(87,0); return; }
		const auto count=raven::darwin::processor_count(call.pointer_width() == 4);
		const uint32_t needed=count*32;
		call.store<uint32_t>(call[2],needed);
		if (!call[0] || call.u32(1)<needed) { call.fail(122,0); return; }
		for (uint32_t i=0;i<count;++i) {
			const auto at=call[0]+i*32;
			call.write(at,std::vector<uint8_t>(32));
			call.store<uint32_t>(at,32); // SYSTEM_CPU_SET_INFORMATION.Size
			call.store<uint32_t>(at+8,i); // CpuSet.Id
			call.store<uint8_t>(at+14,uint8_t(i)); // LogicalProcessorIndex
			call.store<uint8_t>(at+15,uint8_t(i)); // CoreIndex
		}
		call.ret(1);
	});
}

// No approximate SID hashing: callers must observe that this service is absent.
void register_appcontainer_api(raven::ApiRegistry &registry)
{
	registry.add("DeriveAppContainerSidFromAppContainerName", 2, [](raven::ApiCall &call) {
		if (call[1]) call.store_pointer(call[1], 0);
		call.fail(120, 0x80070078U); // HRESULT_FROM_WIN32(ERROR_CALL_NOT_IMPLEMENTED)
	});
}

void seed_hardware_registry(raven::NativeRegistry &hive)
{
	for (long cpu = 0; cpu < sysconf(_SC_NPROCESSORS_ONLN); ++cpu) {
		const auto path = u"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\" + raven::from_ansi(std::to_string(cpu), 1252);
		uint64_t key = 0;
		uint32_t disposition = 0;
		if (hive.create(0x80000002, path, key, disposition))
			continue;
		const auto text = [&](const char16_t *name, const std::string &value) {
			const auto wide = raven::from_ansi(value, 1252) + char16_t(0);
			const auto data = std::span(reinterpret_cast<const uint8_t *>(wide.data()), wide.size() * 2);
			uint32_t type = 0;
			std::vector<uint8_t> previous;
			if (hive.query(key, name, type, previous) || type != 1 || !std::equal(previous.begin(), previous.end(), data.begin(), data.end()))
				hive.set(key, name, 1, data);
		};
		unsigned a, b, c, d;
		__get_cpuid(1, &a, &b, &c, &d);
		// The Windows CPU identity agrees with the x86 CPUID actually executed by Rosetta, rather than describing an ARM ISA.
		text(u"Identifier", "Intel64 Family " + std::to_string((a >> 8) & 15) + " Model " +
			std::to_string(((a >> 4) & 15) | ((a >> 12) & 0xf0)) + " Stepping " + std::to_string(a & 15));
		text(u"VendorIdentifier", "GenuineIntel");
		text(u"ProcessorNameString", cpu_brand() + " (Rosetta 2 x86_64)");
		const uint32_t mhz = cpu_frequency(unsigned(cpu)).maximum;
		uint32_t type = 0;
		std::vector<uint8_t> previous;
		if (hive.query(key, u"~MHz", type, previous) || type != 4 || previous.size() != sizeof mhz || std::memcmp(previous.data(), &mhz, sizeof mhz))
			hive.set(key, u"~MHz", 4, std::span(reinterpret_cast<const uint8_t *>(&mhz), sizeof mhz));
		hive.close(key);
	}
}

}

namespace raven_darwin {
void register_host_extensions(raven::ApiRegistry &registry)
{
	// The macOS network adapter set is currently a snapshot. Allow callers to subscribe and
	// cancel without claiming a change; this also lets browsers continue their audio startup.
	for (const auto *name : {"NotifyIpInterfaceChange","NotifyUnicastIpAddressChange","NotifyRouteChange2"})
		registry.add(name,5,[](raven::ApiCall &call) {
			if (!call[1] || !call[4]) { call.ret(87); return; }
			call.store_pointer(call[4],1); call.ret(0);
		});
	registry.add("CancelMibChangeNotify2",1,[](raven::ApiCall &call) { call.ret(call[0]?0:87); });
	// There is no Windows IME installed in a Darwin prefix.
	registry.add("ImmGetDescriptionW", 3, [](raven::ApiCall &call) { call.ret(0); });
	registry.add("ImmGetDescriptionA", 3, [](raven::ApiCall &call) { call.ret(0); });
}
}
