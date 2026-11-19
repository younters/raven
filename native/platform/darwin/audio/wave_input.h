#pragma once
#include "api.h"
#include "output.h"
#include <optional>
namespace raven_darwin {
using WaveFormatReader = std::optional<AudioFormat> (*)(raven::ApiCall &, uint64_t);
void register_wave_input(raven::ApiRegistry &, raven::KernelContext &, WaveFormatReader);
}
