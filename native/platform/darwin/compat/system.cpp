#include <raven_darwin_system.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOKitKeys.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <net/if.h>
#include <net/if_media.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <uuid/uuid.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace {

uint64_t number(const std::string &name)
{
	uint64_t value = 0;
	size_t size = sizeof value;
	return sysctlbyname(name.c_str(), &value, &size, nullptr, 0) == 0 ? value : 0;
}

std::string text(const char *name)
{
	size_t size = 0;
	if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || !size)
		return {};
	std::string value(size, '\0');
	if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0)
		return {};
	value.resize(strnlen(value.c_str(), size));
	return value;
}

std::string property(io_registry_entry_t entry, CFStringRef key)
{
	CFTypeRef value = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
	if (!value)
		return {};
	std::string result;
	if (CFGetTypeID(value) == CFStringGetTypeID()) {
		char buffer[256];
		if (CFStringGetCString(static_cast<CFStringRef>(value), buffer, sizeof buffer, kCFStringEncodingUTF8))
			result = buffer;
	} else if (CFGetTypeID(value) == CFDataGetTypeID()) {
		const auto bytes = static_cast<CFDataRef>(value);
		const auto *begin = reinterpret_cast<const char *>(CFDataGetBytePtr(bytes));
		result.assign(begin, strnlen(begin, size_t(CFDataGetLength(bytes))));
	}
	CFRelease(value);
	return result;
}

}

namespace raven_darwin {

std::string cpu_brand()
{
	return text("machdep.cpu.brand_string");
}

CpuFrequency cpu_frequency(unsigned processor)
{
	// Intel Macs publish hw.cpufrequency; some systems publish frequency by performance level instead. Apple silicon often publishes
	// neither. Zero then means unknown, rather than inventing a GHz value or confusing the time-stamp-counter rate with core frequency.
	static const auto frequencies = [] {
		std::vector<CpuFrequency> result;
		const uint64_t maximum = number("hw.cpufrequency_max"), current = number("hw.cpufrequency");
		const unsigned levels = unsigned(std::min<uint64_t>(number("hw.nperflevels"), 32));
		for (unsigned level = 0; level < levels; ++level) {
			const auto prefix = "hw.perflevel" + std::to_string(level) + ".";
			const auto max = number(prefix + "cpufrequency_max"), now = number(prefix + "cpufrequency");
			const auto maximum_hz = max ? max : now ? now : maximum ? maximum : current;
			const CpuFrequency frequency{uint32_t(maximum_hz / 1000000), uint32_t((now ? now : current ? current : maximum_hz) / 1000000)};
			result.insert(result.end(), size_t(std::min<uint64_t>(number(prefix + "logicalcpu"), 4096)), frequency);
		}
		if (result.empty())
			result.resize(size_t(std::max<long>(1, sysconf(_SC_NPROCESSORS_ONLN))),
				CpuFrequency{uint32_t((maximum ? maximum : current) / 1000000), uint32_t((current ? current : maximum) / 1000000)});
		return result;
	}();
	return frequencies[processor % frequencies.size()];
}

bool wireless_interface(const char *name)
{
	// Wi-Fi presents an Ethernet link address on macOS. SystemConfiguration retains its physical interface type even while it is off.
	if (const auto interfaces = SCNetworkInterfaceCopyAll()) {
		bool wireless = false;
		for (CFIndex index = 0; index < CFArrayGetCount(interfaces); ++index) {
			const auto interface = static_cast<SCNetworkInterfaceRef>(CFArrayGetValueAtIndex(interfaces, index));
			const auto bsd_name = SCNetworkInterfaceGetBSDName(interface), type = SCNetworkInterfaceGetInterfaceType(interface);
			char buffer[IFNAMSIZ];
			if (bsd_name && type && CFStringGetCString(bsd_name, buffer, sizeof buffer, kCFStringEncodingUTF8) &&
				!std::strcmp(buffer, name) && CFEqual(type, kSCNetworkInterfaceTypeIEEE80211)) {
				wireless = true;
				break;
			}
		}
		CFRelease(interfaces);
		if (wireless)
			return true;
	}
	const int probe = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (probe < 0)
		return false;
	ifmediareq media{};
	std::strncpy(media.ifm_name, name, IFNAMSIZ - 1);
	const bool wireless = ioctl(probe, SIOCGIFMEDIA, &media) == 0 && IFM_TYPE(media.ifm_current) == IFM_IEEE80211;
	close(probe);
	return wireless;
}

std::vector<uint8_t> smbios_table()
{
	std::string model = text("hw.model"), platform_uuid;
	const io_service_t platform = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOPlatformExpertDevice"));
	if (platform) {
		if (auto value = property(platform, CFSTR("model")); !value.empty())
			model = std::move(value);
		platform_uuid = property(platform, CFSTR(kIOPlatformUUIDKey));
		IOObjectRelease(platform);
	}
	std::vector<uint8_t> table(0x1b, 0);
	table[0] = 1; table[1] = 0x1b; table[2] = 1;
	table[4] = 1; table[5] = model.empty() ? 0 : 2;
	// Type 1 deliberately leaves serial-number index zero. SMBIOS 2.6+ stores the UUID's first three fields little endian.
	uuid_t uuid{};
	if (uuid_parse(platform_uuid.c_str(), uuid) == 0) {
		const unsigned order[] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
		for (unsigned i = 0; i < 16; ++i)
			table[8 + i] = uuid[order[i]];
	}
	table[24] = 2; // Unknown wake-up type.
	const auto string = [&](const std::string &value) {
		table.insert(table.end(), value.begin(), value.end());
		table.push_back(0);
	};
	string("Apple Inc.");
	if (!model.empty())
		string(model);
	table.push_back(0);
	// One Type 17 device describes unified physical memory, using the extended MiB field for machines with >= 32 GiB.
	const uint64_t mib = number("hw.memsize") / (1024 * 1024);
	const size_t at = table.size();
	table.resize(at + 0x28, 0);
	table[at] = 17; table[at + 1] = 0x28; table[at + 2] = 2;
	table[at + 4] = 0xff; table[at + 5] = 0xff;
	table[at + 6] = 0xfe; table[at + 7] = 0xff;
	const uint16_t size = uint16_t(mib < 0x7fff ? mib : 0x7fff);
	std::memcpy(table.data() + at + 12, &size, sizeof size);
	if (mib >= 0x7fff) {
		const uint32_t extended = uint32_t(std::min<uint64_t>(mib, 0x7fffffff));
		std::memcpy(table.data() + at + 28, &extended, sizeof extended);
	}
	table[at + 14] = 2; table[at + 18] = 2; // Unknown form factor and memory type.
	table.insert(table.end(), {0, 0, 127, 4, 0xff, 0xff, 0, 0});
	return table;
}

}
