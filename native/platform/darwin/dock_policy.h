#pragma once
#include "desktop/presentation.h"

namespace raven {
// Guest facts needed by hosts that expose a taskbar/Dock application only for a real window.
struct DesktopWindowPresence {
	bool visible = false, top_level = false, message_only = false, owner_visible = false;
	uint32_t style = 0, ex_style = 0;
	uint64_t owner = 0;
	int32_t left = 0, top = 0, right = 0, bottom = 0;
};

}

namespace raven::darwin {
inline bool dock_eligible(const DesktopWindowPresence &facts)
{
	const bool app_window = facts.ex_style & 0x40000U;
	return facts.visible && facts.top_level && !(facts.style & 0x40000000U) && !facts.message_only &&
		(!(facts.ex_style & 0x80U) || app_window) && (!facts.owner || app_window);
}
}
