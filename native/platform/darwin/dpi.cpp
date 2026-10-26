// macOS currently exposes one desktop DPI per process. Keep the Windows coordinate space in device pixels only for aware programs.
#include "desktop/windows.h"
#include "nt/actctx.h"
#include "desktop/canvas.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <charconv>
#include "raven_darwin_dpi.h"

namespace raven {

#ifdef RAVEN_SKIA
std::array<uint8_t,92> NativeWindows::interface_font() const { return paint::interface_font(objects.nonclient_metrics().dpi); }
int32_t NativeWindows::text_width(TextRenderer &renderer,std::u16string_view text) const
{
	return paint::text_width(renderer,text,objects.nonclient_metrics().dpi);
}
int32_t NativeWindows::draw_text(TextRenderer &renderer,paint::Canvas &canvas,std::u16string_view text,int32_t x,int32_t y,uint32_t color,
	std::optional<size_t> underline,int32_t limit) const
{
	return paint::draw_text(renderer,canvas,text,x,y,color,underline,limit,objects.nonclient_metrics().dpi);
}
#endif

void NativeWindows::initialize_dpi()
{
	if (!presentation.backing_scale) return;
	display_scale=std::clamp(presentation.backing_scale(),1.0,4.0);
	if (const char *text=std::getenv("RAVEN_DESKTOP_DPI")) {
		uint32_t value=0;
		const auto end=text+std::char_traits<char>::length(text);
		const auto parsed=std::from_chars(text,end,value);
		if (parsed.ec==std::errc{} && parsed.ptr==end && value>=96 && value<=384) {
			inherited_dpi=value;
			display_scale=double(value)/96;
		}
	}
	const auto manifest=activation_contexts().process_dpi_context();
	dpi_context=manifest.value_or(-1);
	dpi_declared=manifest.has_value();
	apply_dpi();
}

void NativeWindows::apply_dpi()
{
	const double scale=inherited_dpi ? double(inherited_dpi)/96 : dpi_context==-1 || dpi_context==-5 ? 1 : display_scale;
	objects.set_dpi(uint32_t(std::lround(96*scale)));
	raven_darwin::child_desktop_dpi=objects.nonclient_metrics().dpi;
	if (presentation.render_scale) presentation.render_scale(scale);
	refresh_screen();
	if (system_table) set_system_table(system_table);
}

void NativeWindows::initialize_window_dpi(uint64_t handle)
{
	auto *window=objects.window(handle);
	if (!presentation.backing_scale || !window || window->parent || !initialized_dpi_windows.insert(handle).second) return;
	const auto dpi=objects.nonclient_metrics().dpi;
	if (dpi==96) return;
	// Saved startup bounds are in the desktop's 96-DPI units. Apply the first host scale after the program finishes creating its controls.
	// Dialogs, including unowned installer dialogs, already derive their dimensions from their scaled dialog font; maximized windows
	// use the output's pixel dimensions. Scaling a dialog again enlarges only its frame and leaves its controls in the upper left.
	const auto *definition=objects.window_class(window->class_id);
	const bool dialog=definition && definition->name=="#32770";
	if (!dialog && !window->owner && (window->style & 0xc00000U)==0xc00000U && !(window->style & 0x21000000U)) {
		const auto rect=window->restore.right>window->restore.left ? window->restore : placement_rect(window->outer,true);
		if (rect.right-rect.left<=8192 && rect.bottom-rect.top<=8192) reshape(handle,rect);
	}
	window=objects.window(handle);
	if (!window) return;
	const auto rect=window->outer;
	const auto address=scratch_allocate(16);
	process.write_memory(address,std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&rect),16));
	send(handle,0x2e0,dpi | (uint64_t(dpi)<<16),address);
}

DesktopRect NativeWindows::placement_rect(DesktopRect rect,bool to_device) const
{
	if (!presentation.backing_scale) return rect;
	// WINDOWPLACEMENT is persistent workspace geometry: retain the host's points across Retina/non-Retina launches.
	// GetWindowRect, SetWindowPos and input continue to use the program's device-pixel coordinate space.
	const auto dpi=objects.nonclient_metrics().dpi;
	const auto scale=[&](int32_t value) { return int32_t(int64_t(value)*(to_device ? dpi : 96)/(to_device ? 96 : dpi)); };
	return {scale(rect.left),scale(rect.top),scale(rect.right),scale(rect.bottom)};
}

void NativeWindows::register_dpi_api(ApiRegistry &registry)
{
	const auto dpi=[this] { return objects.nonclient_metrics().dpi; };
	const auto context=[](ApiCall &call) { return call.pointer_width()==4 ? int64_t(call.i32(0)) : int64_t(call[0]); };
	const auto valid=[](int64_t value) { return value>=-5 && value<=-1; };
	const auto awareness=[](int64_t value) -> uint32_t { return value==-1 || value==-5 ? 0 : value==-2 ? 1 : 2; };
	registry.add("IsValidDpiAwarenessContext",1,[=](ApiCall &call) { call.ret(valid(context(call))); });
	const auto set_process=[this](int64_t value) {
		if (dpi_declared || !objects.window_handles().empty()) return false;
		dpi_context=value;
		dpi_declared=true;
		apply_dpi();
		return true;
	};
	registry.add("SetProcessDPIAware",0,[=](ApiCall &call) { call.ret(set_process(-2)); });
	registry.add("IsProcessDPIAware",0,[=,this](ApiCall &call) { call.ret(awareness(dpi_context)!=0); });
	registry.add("SetProcessDpiAwarenessContext",1,[=](ApiCall &call) {
		const auto value=context(call);
		if (!valid(value)) { call.fail(87); return; }
		if (!set_process(value)) { call.fail(5); return; }
		call.ret(1);
	});
	registry.add("SetProcessDpiAwareness",1,[=](ApiCall &call) {
		if (call.u32(0)>2) { call.ret(0x80070057); return; }
		call.ret(set_process(-1-int64_t(call.u32(0))) ? 0 : 0x80070005);
	});
	registry.add("GetProcessDpiAwareness",2,[=,this](ApiCall &call) {
		if (!call[1]) { call.ret(0x80070057); return; }
		call.store<uint32_t>(call[1],awareness(dpi_context)); call.ret(0);
	});
	registry.add("GetDpiAwarenessContextForProcess",1,[this](ApiCall &call) { call.ret(uint64_t(dpi_context)); });
	registry.add("GetWindowDpiAwarenessContext",1,[this](ApiCall &call) {
		if (!objects.window(call[0])) { call.fail(1400); return; }
		call.ret(uint64_t(dpi_context));
	});
	registry.add("GetThreadDpiAwarenessContext",0,[this](ApiCall &call) {
		const auto found=thread_dpi_contexts.find(environment.thread_id());
		call.ret(uint64_t(found==thread_dpi_contexts.end() ? dpi_context : found->second));
	});
	registry.add("SetThreadDpiAwarenessContext",1,[=,this](ApiCall &call) {
		const auto value=context(call);
		if (!valid(value)) { call.fail(87); return; }
		const auto me=environment.thread_id();
		const auto found=thread_dpi_contexts.find(me);
		const auto previous=found==thread_dpi_contexts.end() ? dpi_context : found->second;
		thread_dpi_contexts[me]=value;
		call.ret(uint64_t(previous));
	});
	registry.add("GetDpiForSystem",0,[this](ApiCall &call) { call.ret(effective_dpi()); });
	registry.add("GetDpiForWindow",1,[=,this](ApiCall &call) {
		call.ret(objects.window(call[0]) ? dpi() : 0);
	});
	registry.add("GetSystemDpiForProcess",1,[=](ApiCall &call) { call.ret(dpi()); });
	registry.add("GetDpiForMonitor",4,[=](ApiCall &call) {
		if (!call[2] || !call[3] || call.u32(1)>2) { call.ret(0x80070057); return; }
		call.store<uint32_t>(call[2],dpi()); call.store<uint32_t>(call[3],dpi()); call.ret(0);
	});
	registry.add("GetScaleFactorForMonitor",2,[this](ApiCall &call) {
		if (!call[1]) { call.ret(0x80070057); return; }
		call.store<uint32_t>(call[1],uint32_t(std::lround(100*display_scale))); call.ret(0);
	});
	registry.add("GetScaleFactorForDevice",1,[this](ApiCall &call) { call.ret(uint32_t(std::lround(100*display_scale))); });
	registry.add("GetSystemMetricsForDpi",2,[this](ApiCall &call) {
		if (!call.u32(1) || call.u32(1)>768) { call.fail(87); return; }
		call.ret(system_metric(objects.nonclient_metrics().at_dpi(call.u32(1)),call.i32(0)));
	});
}

}
