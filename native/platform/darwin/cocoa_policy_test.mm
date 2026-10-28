#import <AppKit/AppKit.h>
#include "cocoa.h"
#include "desktop/objects.h"
#include "gui_session.h"
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

int main()
{
	if (!raven_macos_gui_available()) {
		fprintf(stderr,"SKIP: Cocoa policy test requires a GUI session and WindowServer\n");
		return 77;
	}
	NSRunningApplication *launch_context=NSWorkspace.sharedWorkspace.frontmostApplication;
	raven::CocoaPresentation host;
	auto ui=host.callbacks();
	ui.host_display();
	auto check=[](bool result) { if (!result) throw std::runtime_error("Cocoa Dock transition failed"); };
	auto policy=[&](NSApplicationActivationPolicy expected) { check(NSApp.activationPolicy==expected); };
	policy(NSApplicationActivationPolicyAccessory); // Connected helpers and delayed splash have no Dock item.
	raven::DesktopWindowPresence facts{.visible=true,.top_level=true,.left=10,.top=10,.right=110,.bottom=90};
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
	// A SetParent hosting anchor is not GW_OWNER.
	raven::DesktopWindow anchor{0,0,u"Hosting anchor",10,10,100,80};
	anchor.visible=true; anchor.style|=0x10000000U;
	const auto anchor_handle=objects.add(anchor);
	record.visible=true; record.style|=0x10000000U; record.shown_on=anchor_handle;
	const auto hosted=objects.add(record);
	ui.window_state(hosted,objects); ui.present(hosted,"Unowned hosted window",100,80,pixels);
	policy(NSApplicationActivationPolicyRegular);
	ui.remove(hosted); ui.poll(std::chrono::milliseconds(0)); policy(NSApplicationActivationPolicyAccessory);
	// Replacing a popup's Cocoa surface must retain eligibility recorded just before present().
	facts.owner=0; facts.ex_style=0; show(100);
	facts.owner=100; host.describe(101,facts); ui.owned_popup(101,100,0,0,100,80,pixels);
	facts.owner=0; host.describe(101,facts); ui.present(101,"Popup becomes top level",100,80,pixels);
	ui.remove(100); ui.poll(std::chrono::milliseconds(0)); policy(NSApplicationActivationPolicyRegular);
	ui.remove(101); ui.poll(std::chrono::milliseconds(0)); policy(NSApplicationActivationPolicyAccessory);
	// A tool splash and its focus notifications cannot take the main window's launch right.
	[launch_context activateWithOptions:NSApplicationActivateAllWindows];
	for (unsigned attempt=0; NSApp.active && attempt<20; ++attempt) ui.poll(std::chrono::milliseconds(5));
	check(!NSApp.active);
	char ticket[]="/tmp/raven-splash-ticket-XXXXXX";
	const int fd=mkstemp(ticket);
	check(fd>=0);
	const pid_t origin=NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
	char value[64];
	const int length=snprintf(value,sizeof(value),"%d\n0\n%d\n",origin,getpid());
	check(write(fd,value,size_t(length))==length); close(fd);
	setenv("RAVEN_FOREGROUND_TICKET",ticket,1);
	facts.ex_style=0x80U; show(200); ui.foreground(200); ui.poll(std::chrono::milliseconds(0)); ui.foreground(200);
	policy(NSApplicationActivationPolicyAccessory);
	check(access(ticket,F_OK)==0);
	facts.ex_style=0; show(201); policy(NSApplicationActivationPolicyRegular);
	check(access(ticket,F_OK)!=0);
	ui.remove(200); ui.remove(201); ui.poll(std::chrono::milliseconds(0));
	policy(NSApplicationActivationPolicyAccessory);
	return 0;
}
