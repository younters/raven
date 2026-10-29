#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace raven::darwin_dxgi {

struct Identity {
	uint16_t vendor;
	uint16_t device;
	const char *description;
};

inline std::optional<Identity> identity()
{
	const char *raw = std::getenv("RAVEN_DXGI_VENDOR");
	if (!raw || !*raw) return std::nullopt;
	const std::string_view value(raw);
	if (value == "amd") return Identity{0x1002, 0x7340, "AMD Radeon Pro 5300M"};
	if (value == "nvidia") return Identity{0x10de, 0x1e84, "NVIDIA GeForce RTX 2070"};
	if (value == "intel") return Identity{0x8086, 0x3e92, "Intel UHD Graphics 630"};
	const auto digits = value.starts_with("0x") ? value.substr(2) : value;
	if (digits.size() != 4) return std::nullopt;
	uint16_t vendor = 0;
	for (const char c : digits) {
		vendor <<= 4;
		if (c >= '0' && c <= '9') vendor |= uint16_t(c - '0');
		else if (c >= 'a' && c <= 'f') vendor |= uint16_t(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') vendor |= uint16_t(c - 'A' + 10);
		else return std::nullopt;
	}
	return Identity{vendor, 1, "Raven Metal GPU"};
}

inline std::string pci_id(Identity value)
{
	char text[64];
	std::snprintf(text, sizeof text, "PCI\\VEN_%04X&DEV_%04X&SUBSYS_00000000", value.vendor, value.device);
	return text;
}

inline std::string dxmt_config(Identity value)
{
	char text[128];
	std::snprintf(text, sizeof text, "dxgi.customVendorId = %04X;dxgi.customDeviceId = %04X;dxgi.customDeviceDesc = %s",
		value.vendor, value.device, value.description);
	return text;
}

}
