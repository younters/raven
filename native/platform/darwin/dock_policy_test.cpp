#include "desktop/presentation.h"
#include <stdexcept>

int main()
{
	using raven::DesktopWindowPresence;
	DesktopWindowPresence window{.visible=true,.top_level=true,.left=10,.top=10,.right=400,.bottom=300,.screen_width=1920,.screen_height=1080};
	auto check=[](bool value) { if (!value) throw std::runtime_error("Dock eligibility contract failed"); };
	check(window.eligible());
	window.visible=false; check(!window.eligible()); window.visible=true;
	window.owner_visible=true; check(!window.eligible()); window.owner_visible=false;
	window.ex_style=0x80; check(!window.eligible()); window.ex_style=0;
	window.top_level=false; check(!window.eligible()); window.top_level=true;
	window.message_only=true; check(!window.eligible()); window.message_only=false;
	window.right=window.left; check(!window.eligible()); window.right=400;
	window.left=1920; check(!window.eligible()); window.left=10;
	window.style=0x20000000; check(window.eligible()); // A minimized taskbar window keeps its place.
}
