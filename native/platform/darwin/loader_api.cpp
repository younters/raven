#include "nt/module_api.h"
#include <cstring>

namespace raven {

bool NativeModuleAPI::dispatch_darwin(std::string_view name)
{
    if (name != "LdrGetDllHandle" && name != "RtlFindExportedRoutineByName") return false;
    // NT loader routines do not change Win32 last error.
    const auto saved = environment.last_error();
    if (name == "RtlFindExportedRoutineByName") {
        const auto args = process.api_arguments(2);
        const auto address = args[1] > UINT16_MAX ? procedure(args[0], args[1]) : 0;
        environment.set_last_error(saved);
        process.return_api(address);
        return true;
    }
    const auto args = process.api_arguments(4);
    if (!args[2] || !args[3]) {
        process.return_api(0xc000000dU);
        return true;
    }
    const bool x86 = process.mode() == NativeMode::x86;
    const auto header = process.read_memory(args[2], x86 ? 8 : 16);
    uint16_t length, maximum;
    std::memcpy(&length, header.data(), 2);
    std::memcpy(&maximum, header.data() + 2, 2);
    uint64_t buffer = 0;
    std::memcpy(&buffer, header.data() + (x86 ? 4 : 8), x86 ? 4 : 8);
    if ((length & 1) || length > maximum || (length && !buffer)) {
        process.return_api(0xc000000dU);
        return true;
    }
    const auto bytes = length ? process.read_memory(buffer, length) : std::vector<uint8_t>{};
    std::string decoded;
    bool ascii = true;
    for (size_t i = 0; i < bytes.size(); i += 2) {
        ascii &= bytes[i + 1] == 0 && bytes[i] != 0;
        decoded.push_back(char(bytes[i]));
    }
    const uint64_t module = module_handle_name(ascii ? std::optional(decoded) : std::nullopt);
    environment.set_last_error(saved);
    if (module)
        process.write_memory(args[3], {reinterpret_cast<const uint8_t *>(&module), x86 ? size_t(4) : size_t(8)});
    process.return_api(module ? 0 : 0xc0000135U);
    return true;
}
}
