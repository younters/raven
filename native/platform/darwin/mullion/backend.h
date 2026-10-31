#pragma once
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace raven {
inline bool mullion_selected()
{
    const char *backend = std::getenv("RAVEN_D3D12_BACKEND");
    return backend && std::string_view(backend) == "mullion";
}
inline std::filesystem::path mullion_root()
{
    if (const char *root = std::getenv("RAVEN_MULLION_ROOT")) {
        if (*root) return root;
    }
#ifdef RAVEN_MULLION_ROOT
    if (std::string_view(RAVEN_MULLION_ROOT).size()) return RAVEN_MULLION_ROOT;
#endif
    throw std::runtime_error("Mullion selected but RAVEN_MULLION_ROOT is unset");
}
}
