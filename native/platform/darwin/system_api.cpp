#include <raven_darwin_system.h>
#include "nt/api.h"
#include "nt/registry.h"
#include "nt/text.h"

#include <cpuid.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>

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
