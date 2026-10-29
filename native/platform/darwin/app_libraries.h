#pragma once
#include <string_view>

namespace raven::darwin {
// This redistributable is not a KnownDLL. Let the existing app-local loader
// prefer a compiler beside the program before the System32 fallback.
// ProgramLibraries filters PE32/PE32+ bitness; it does not validate Machine.
inline bool program_may_replace(std::string_view name)
{
	return name == "d3dcompiler_47.dll";
}
}
