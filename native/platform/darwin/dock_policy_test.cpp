#include "dock_policy.h"
#include "desktop/presentation.h"
#include <stdexcept>

int main()
{
	using raven::DesktopWindowPresence;
	DesktopWindowPresence window{.visible=true,.top_level=true,.left=10,.top=10,.right=400,.bottom=300,.screen_width=1920,.screen_height=1080};
	auto check=[](bool value) { if (!value) throw std::runtime_error("Dock eligibility contract failed"); };
	check(raven::darwin::dock_eligible(window));
	window.visible=false; check(!raven::darwin::dock_eligible(window)); window.visible=true;
	window.owner=123; window.owner_visible=true; check(!raven::darwin::dock_eligible(window));
	window.owner_visible=false; check(!raven::darwin::dock_eligible(window)); // Hidden owner still suppresses a taskbar button.
	window.ex_style=0x40000U; check(raven::darwin::dock_eligible(window));
	window.ex_style=0x40080U; check(raven::darwin::dock_eligible(window));
	window.owner=0; window.ex_style=0;
	window.ex_style=0x80; check(!raven::darwin::dock_eligible(window)); window.ex_style=0;
	window.top_level=false; check(!raven::darwin::dock_eligible(window)); window.top_level=true;
	window.style=0x40000000U; check(!raven::darwin::dock_eligible(window)); window.style=0;
	window.message_only=true; check(!raven::darwin::dock_eligible(window)); window.message_only=false;
	window.right=window.left; check(raven::darwin::dock_eligible(window)); window.right=400;
	window.left=1920; check(raven::darwin::dock_eligible(window)); window.left=10;
	window.left=-32000; window.top=-32000; window.right=-31840; window.bottom=-31972;
	window.style=0x20000000; check(raven::darwin::dock_eligible(window)); // A minimized taskbar window keeps its place.
}
