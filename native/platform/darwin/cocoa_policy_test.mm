#import <AppKit/AppKit.h>
#include "cocoa.h"
#include "desktop/objects.h"
#include <stdexcept>

int main()
{
	raven::CocoaPresentation host;
	auto ui=host.callbacks();
	ui.host_display();
	auto check=[](bool result) { if (!result) throw std::runtime_error("Cocoa Dock transition failed"); };
	auto policy=[&](NSApplicationActivationPolicy expected) { check(NSApp.activationPolicy==expected); };
	policy(NSApplicationActivationPolicyAccessory); // Connected helpers and delayed splash have no Dock item.
	raven::DesktopWindowPresence facts{.visible=true,.top_level=true,.left=10,.top=10,.right=110,.bottom=90,.screen_width=800,.screen_height=600};
	std::vector<uint8_t> pixels(100*80*4,255);
	auto show=[&](uint64_t handle) { host.describe(handle,facts); ui.present(handle,"Standalone dialog",100,80,pixels); };
	show(1); policy(NSApplicationActivationPolicyRegular); // A dialog without a local owner qualifies.
	ui.frame(1,{},0x20000000U); ui.minimize(1);
	ui.present(1,"Minimized",100,80,pixels); ui.poll(std::chrono::milliseconds(0));
	policy(NSApplicationActivationPolicyRegular);
	ui.remove(1); policy(NSApplicationActivationPolicyRegular);
	show(2); ui.poll(std::chrono::milliseconds(0)); policy(NSApplicationActivationPolicyRegular); // Replacement in one batch.
	ui.remove(2); ui.poll(std::chrono::milliseconds(0)); policy(NSApplicationActivationPolicyAccessory); // Tray hide.
	show(3); policy(NSApplicationActivationPolicyRegular); // Restore from tray.
	facts.ex_style=0x80U; host.describe(3,facts); ui.poll(std::chrono::milliseconds(0)); policy(NSApplicationActivationPolicyAccessory);
	ui.remove(3);
	facts.ex_style=0x80U; facts.owner=0; facts.owner_visible=false;
	show(4); policy(NSApplicationActivationPolicyAccessory);
	facts.ex_style=0; facts.owner=4; facts.owner_visible=true;
	show(5); policy(NSApplicationActivationPolicyAccessory); // Visible local owner suppresses its dialog's taskbar place.
	ui.remove(4); policy(NSApplicationActivationPolicyAccessory); // Removing or hiding an owner does not add a taskbar button.
	facts.ex_style=0x40080U; host.describe(5,facts); policy(NSApplicationActivationPolicyRegular); // APPWINDOW overrides ownership/tool style.
	ui.remove(5); ui.poll(std::chrono::milliseconds(0)); policy(NSApplicationActivationPolicyAccessory);
	raven::DesktopObjects objects;
	raven::DesktopWindow record{0,0,u"Visibility intent",10,10,100,80};
	const auto handle=objects.add(record);
	ui.window_state(handle,objects); policy(NSApplicationActivationPolicyAccessory);
	ui.visibility(handle,true); ui.window_state(handle,objects);
	ui.present(handle,"Visibility intent",100,80,pixels); policy(NSApplicationActivationPolicyRegular);
	check(!objects.window(handle)->visible); // The backend sees intent without reordering upstream state mutation.
	ui.remove(handle); ui.poll(std::chrono::milliseconds(0)); policy(NSApplicationActivationPolicyAccessory);
	return 0;
}
