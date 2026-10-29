// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "protocol.h"
#include <array>
#include <functional>
#include <span>

namespace raven {
class DesktopObjects;
namespace darwin {
struct CompositionBinding {
	uint64_t target=0; float x=0,y=0; int32_t order=0; float sx=1,sy=1;
	float left=-1000000,top=-1000000,right=1000000,bottom=1000000;
	bool linear=false; std::array<float,4> rounded{}; std::array<float,8> radius{};
};
// Returns S_OK or E_INVALIDARG; validation completes before any properties change.
uint32_t publish_composition(DesktopObjects &objects, uint64_t device, uint32_t magic, uint32_t count,
	std::span<const raven_dcomp_item> items, const std::function<bool(uint64_t)> &foreign_target);
}
}
