#pragma once

#include <cstdint>

namespace raven_darwin {

// Child controllers share their creator's desktop coordinate space, even when their manifests declare different DPI awareness.
inline uint32_t child_desktop_dpi = 0;

}
