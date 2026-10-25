#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace raven { class ApiRegistry; class NativeRegistry; }

namespace raven_darwin {

struct CpuFrequency {
	uint32_t maximum = 0, current = 0;
};

// These describe the host hardware; instruction-set identity still comes from Rosetta's CPUID shim.
std::string cpu_brand();
CpuFrequency cpu_frequency(unsigned processor);
bool wireless_interface(const char *name);
std::vector<uint8_t> smbios_table();
void register_firmware_api(raven::ApiRegistry &registry);
void seed_hardware_registry(raven::NativeRegistry &hive);

}
