#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace raven_darwin {

// Window and memory DCs use the process's effective DPI. Keep other capabilities on the fast path.
inline std::vector<uint8_t> screen_caps_prelude(const std::vector<uint8_t> &code)
{
	auto result=code;
	const std::array<uint8_t,6> fixed{0xb8,0x60,0,0,0,0xc3};
	const auto at=std::search(result.begin(),result.end(),fixed.begin(),fixed.end());
	if (at==result.end()) throw std::runtime_error("missing fixed DPI prelude");
	const auto offset=size_t(at-result.begin());
	std::fill_n(at,6,0x90);
	result[offset]=0xeb; result[offset+1]=uint8_t(result.size()-5-offset-2);
	return result;
}

}
