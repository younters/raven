#pragma once
#include <algorithm>
#include <thread>

namespace raven::darwin {
// One guest group, limited by the guest affinity mask width. WOW64 uses the
// host count too; the old four-CPU x86 dispatcher model was inconsistent.
inline unsigned processor_count(bool x86)
{
	return std::clamp(std::thread::hardware_concurrency(), 1U, x86 ? 32U : 64U);
}
}
