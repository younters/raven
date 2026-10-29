#pragma once
#include <string_view>

namespace raven::darwin {
// This redistributable is not a KnownDLL. Let the existing app-local loader
// prefer a same-width copy beside the program before the System32 fallback.
inline bool program_may_replace(std::string_view name)
{
	return name == "d3dcompiler_47.dll";
}
}
