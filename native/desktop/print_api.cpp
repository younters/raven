#include "api.h"
#include "printing.h"
#include "nt/kernel_context.h"
#include "nt/text.h"
#include <algorithm>
#include <cmath>

namespace raven {
#ifdef __APPLE__
namespace {
std::string printer_name(ApiCall &call, uint64_t address, bool wide)
{
	return address ? to_utf8(wide ? call.wide(address) : from_ansi(call.narrow(address), 0)) : host_default_printer();
}
void validate_printer(const std::string &name)
{
	const auto names = host_printers();
	if (std::find(names.begin(), names.end(), name) == names.end()) throw uint32_t(1801);
}
PrinterSettings read_mode(ApiCall &call, const std::string &name, uint64_t mode, bool wide)
{
	validate_printer(name);
	auto p = host_printer_settings(name);
	if (!mode) return p;
	const size_t base = wide ? 64 : 32;
	if (call.load<uint16_t>(mode + base + 4) < base + 40) throw uint32_t(13);
	const auto fields = call.load<uint32_t>(mode + base + 8);
	if (fields & 2) {
		const auto paper = call.load<uint16_t>(mode + base + 14);
		if (paper == 1) { p.width = 612; p.height = 792; }
		else if (paper == 9) { p.width = 595.276; p.height = 841.89; }
	}
	if (fields & 4) p.height = call.load<uint16_t>(mode + base + 16) * 72.0 / 254;
	if (fields & 8) p.width = call.load<uint16_t>(mode + base + 18) * 72.0 / 254;
	if (fields & 1) p.landscape = call.load<uint16_t>(mode + base + 12) == 2;
	if (p.landscape && p.width < p.height) std::swap(p.width, p.height);
	if (fields & 0x100) p.copies = std::clamp<int>(call.load<uint16_t>(mode + base + 22), 1, 999);
	if (fields & 0x800) p.color = call.load<uint16_t>(mode + base + 28) != 1;
	if (p.width < 72 || p.width > 2880 || p.height < 72 || p.height > 2880) throw uint32_t(87);
	// Keep host hardware margins while changing paper/orientation.
	p.right = p.width - p.left; p.bottom = p.height - p.top;
	return p;
}
size_t mode_size(bool wide) { return wide ? 220 : 156; }
void write_mode(ApiCall &call, uint64_t address, const PrinterSettings &p, bool wide)
{
	call.write(address, std::vector<uint8_t>(mode_size(wide), 0));
	if (wide) call.put_wide(address, from_utf8(p.name).substr(0, 31), 32);
	else call.put_narrow(address, to_ansi(from_utf8(p.name),0).substr(0,31),32);
	const size_t base = wide ? 64 : 32;
	call.store<uint16_t>(address + base, 0x401);
	call.store<uint16_t>(address + base + 2, 1);
	call.store<uint16_t>(address + base + 4, uint16_t(mode_size(wide)));
	call.store<uint32_t>(address + base + 8, 0x1 | 0x2 | 0x4 | 0x8 | 0x100 | 0x400 | 0x800 | 0x2000);
	call.store<uint16_t>(address + base + 12, p.landscape ? 2 : 1);
	call.store<uint16_t>(address + base + 14, std::abs(std::min(p.width,p.height) - 595.276) < 2 ? 9 : 1);
	call.store<uint16_t>(address + base + 16, uint16_t(p.height * 254 / 72));
	call.store<uint16_t>(address + base + 18, uint16_t(p.width * 254 / 72));
	call.store<uint16_t>(address + base + 22, uint16_t(p.copies));
	call.store<uint16_t>(address + base + 26, 300);
	call.store<uint16_t>(address + base + 28, p.color ? 2 : 1);
	call.store<uint16_t>(address + base + 32, 300);
}
template<class F> auto protect(F f)
{
	return [f](ApiCall &call) { try { f(call); } catch (uint32_t error) { call.fail(error); } };
}
}
#endif
void NativeDesktop::register_print_api(ApiRegistry &registry, KernelContext &context)
{
#ifdef __APPLE__
	auto make_dc = [this](const PrinterSettings &settings) {
		const auto w = uint32_t(std::lround((settings.right - settings.left) * 300 / 72));
		const auto h = uint32_t(std::lround((settings.bottom - settings.top) * 300 / 72));
		if (!w || !h || w > 8192 || h > 8192) throw uint32_t(87);
		const auto bitmap = allocate_bitmap(w, h, 32, false, true);
		if (!bitmap) throw uint32_t(8);
		DesktopDC dc{bitmap, bitmap};
		dc.printer = std::make_shared<PrinterDC>(); dc.printer->settings = settings;
		const auto handle = objects.add(std::move(dc));
		if (!handle) { objects.erase(bitmap); throw uint32_t(8); }
		return handle;
	};
	auto handles = std::make_shared<std::map<uint64_t, std::string>>();
	auto next_handle = std::make_shared<uint64_t>(0x70000000);
	for (bool wide : {false, true}) {
		registry.add(wide ? "CreateDCW" : "CreateDCA", 4, protect([=, this](ApiCall &call) {
			const auto driver = call[0] ? printer_name(call, call[0], wide) : "";
			// Monitor enumeration uses CreateDC(nullptr, monitor.szDevice, ...).
			// Treat that as a display query so callers retain the monitor work area.
			if (driver == "DISPLAY" || driver == "display" ||
				(driver.empty() && call[1] && printer_name(call, call[1], wide) == "\\\\.\\DISPLAY1")) { call.ret(create_dc(0)); return; }
			call.ret(make_dc(read_mode(call, printer_name(call, call[1], wide), call[3], wide)));
		}));
		registry.add(wide ? "CreateICW" : "CreateICA", 4, registry.find(wide ? "CreateDCW" : "CreateDCA")->second);
		registry.add(wide ? "GetDefaultPrinterW" : "GetDefaultPrinterA", 2, protect([=, this](ApiCall &call) {
			const auto name = host_default_printer();
			const auto ansi = to_ansi(from_utf8(name),0);
			const auto units = wide ? from_utf8(name).size() + 1 : ansi.size() + 1;
			const auto capacity = call.load<uint32_t>(call[1]); call.store<uint32_t>(call[1], uint32_t(units));
			if (!call[0] || capacity < units) { call.fail(122); return; }
			if (wide) call.put_wide(call[0], from_utf8(name), units); else call.put_narrow(call[0], ansi, units);
			call.ret(1);
		}));
		// Printer handles are process-local identifiers, kept separate from GDI handles.
		registry.add(wide ? "OpenPrinterW" : "OpenPrinterA", 3, protect([=, this](ApiCall &call) {
			const auto name = printer_name(call, call[0], wide); validate_printer(name);
			const uint64_t handle = (*next_handle)++;
			(*handles)[handle] = name; call.store_pointer(call[1], handle); call.ret(1);
		}));
		// ClosePrinter accepts both widths; the registry below installs one shared implementation after the loop.
		registry.add(wide ? "DocumentPropertiesW" : "DocumentPropertiesA", 6, protect([=, this](ApiCall &call) {
			if (call[1] && !handles->contains(call[1])) throw uint32_t(6);
			const auto name = call[2] ? printer_name(call, call[2], wide) : (handles->contains(call[1]) ? handles->at(call[1]) : host_default_printer());
			validate_printer(name);
			if (!call.u32(5)) { call.ret(mode_size(wide)); return; }
			auto p = read_mode(call, name, call.u32(5) & 8 ? call[4] : 0, wide);
			if ((call.u32(5) & 4) && !host_print_panel(p, true)) { call.ret(2); return; }
			if ((call.u32(5) & 2) && call[3]) write_mode(call, call[3], p, wide);
			call.ret(1);
		}));
		auto printer_info = [wide](ApiCall &call, const std::vector<std::string> &names, uint32_t level, uint64_t buffer, uint32_t size, uint64_t needed_at) {
			const auto pw = call.pointer_width();
			if (level != 1 && level != 2 && level != 4 && level != 5) throw uint32_t(124);
			const size_t stride = level == 4 ? (pw == 8 ? 24 : 12) : level == 5 ? (pw == 8 ? 32 : 20) : level == 2 ? (pw == 8 ? 136 : 84) : (pw == 8 ? 32 : 16);
			std::vector<std::vector<std::pair<size_t,std::u16string>>> fields;
			size_t needed = stride * names.size();
			for (const auto &name : names) {
				std::vector<std::pair<size_t,std::u16string>> row;
				if (level == 1) row = {{pw == 8 ? 8 : 4,from_utf8(name)}, {pw == 8 ? 16 : 8,from_utf8(name)}, {pw == 8 ? 24 : 12,u"Raven printer"}};
				else {
					row.push_back({level == 2 ? pw : 0,from_utf8(name)});
					if (level == 5 || level == 2) row.push_back({level == 2 ? pw*3 : pw,u"PORTPROMPT:"});
					if (level == 2) {
						row.push_back({pw*4,u"Raven GDI"}); row.push_back({pw*10,u"RAW"}); row.push_back({pw*9,u"winprint"});
					}
				}
				for (const auto &[offset,text] : row) needed += (wide ? (text.size()+1)*2 : to_ansi(text,0).size()+1);
				fields.push_back(std::move(row));
			}
			call.store<uint32_t>(needed_at,uint32_t(needed));
			if (!buffer || size < needed) { call.fail(122); return false; }
			call.write(buffer,std::vector<uint8_t>(needed,0));
			uint64_t at = buffer + stride * names.size();
			for (size_t n=0; n<names.size(); ++n) {
				const auto row = buffer+n*stride;
				for (const auto &[offset,text] : fields[n]) {
					call.store_pointer(row+offset,at);
					if (wide) { call.put_wide(at,text,text.size()+1); at+=(text.size()+1)*2; }
					else { auto ansi=to_ansi(text,0); call.put_narrow(at,ansi,ansi.size()+1); at+=ansi.size()+1; }
				}
				if (level == 4 || level == 5) call.store<uint32_t>(row+pw*2,0x40);
				if (level == 2) { call.store<uint32_t>(row+pw*13,0x40); call.store<uint32_t>(row+pw*13+4,1); call.store<uint32_t>(row+pw*13+8,1); }
			}
			call.ret(1); return true;
		};
		registry.add(wide ? "EnumPrintersW" : "EnumPrintersA", 7, protect([=, this](ApiCall &call) {
			call.store<uint32_t>(call[6],0);
			const auto names = host_printers();
			if (printer_info(call,names,call.u32(2),call[3],call.u32(4),call[5])) call.store<uint32_t>(call[6],uint32_t(names.size()));
		}));
		registry.add(wide ? "GetPrinterW" : "GetPrinterA", 5, protect([=, this](ApiCall &call) {
			if (!handles->contains(call[0])) throw uint32_t(6);
			printer_info(call,{handles->at(call[0])},call.u32(1),call[2],call.u32(3),call[4]);
		}));
		registry.add(wide ? "DeviceCapabilitiesW" : "DeviceCapabilitiesA", 5, protect([=, this](ApiCall &call) {
			const auto p = read_mode(call,printer_name(call,call[0],wide),call[4],wide);
			switch (call.u32(2)) {
			case 1: call.ret(0x250f); break; // DC_FIELDS
			case 2: // DC_PAPERS
				if (call[3]) {call.store<uint16_t>(call[3],1);call.store<uint16_t>(call[3]+2,9);} call.ret(2); break;
			case 3: // DC_PAPERSIZE, tenths of a millimetre
				if (call[3]) {call.store<int32_t>(call[3],2159);call.store<int32_t>(call[3]+4,2794);call.store<int32_t>(call[3]+8,2100);call.store<int32_t>(call[3]+12,2970);} call.ret(2); break;
			case 7: call.ret(mode_size(wide)); break;
			case 8: call.ret(0); break;
			case 10: call.ret(1); break;
			case 11: call.ret(0x401); break;
			case 13: if (call[3]) {call.store<int32_t>(call[3],300);call.store<int32_t>(call[3]+4,300);} call.ret(1); break;
			case 16:
				if (call[3]) {
					call.write(call[3],std::vector<uint8_t>(128*(wide?2:1),0));
					if (wide) {call.put_wide(call[3],u"Letter",64);call.put_wide(call[3]+128,u"A4",64);}
					else {call.put_narrow(call[3],"Letter",64);call.put_narrow(call[3]+64,"A4",64);}
				} call.ret(2); break;
			case 17: call.ret(90); break;
			case 18: call.ret(999); break;
			case 32: call.ret(p.color); break;
			default: call.ret(uint32_t(-1)); break;
			}
		}));
		registry.add(wide ? "StartDocW" : "StartDocA", 2, protect([=, this, &context](ApiCall &call) {
			auto *dc = objects.dc(call[0]);
			if (!dc || !dc->printer) throw uint32_t(6);
			auto &p = *dc->printer;
			if (p.document) throw uint32_t(183);
			const size_t offset = call.pointer_width();
			const auto title = call.load_pointer(call[1] + offset), output = call.load_pointer(call[1] + offset * 2);
			std::string path;
			if (output) {
				const auto file = wide ? call.wide(output) : from_ansi(call.narrow(output), 0);
				const auto resolved = context.paths.resolve(file);
				if (!resolved) throw uint32_t(3);
				path = resolved->host.string();
			}
			p.document = host_print_document(p.settings, title ? printer_name(call, title, wide) : "Raven document", path);
			p.page_open = false; call.ret(1);
		}));
		auto dialog = [=, this, &context](ApiCall &call, bool extended, bool setup) {
			const auto a = call[0]; const size_t pw = call.pointer_width();
			// PRINTDLG is packed to one byte on x86 and naturally aligned on x64; PAGESETUPDLG has the same prefix.
			const size_t first = pw == 8 ? 8 : 4, dm = first + pw, dn = dm + pw, hdc = dn + pw;
			const size_t flags_at = setup ? hdc : hdc + pw;
			const auto flags = call.load<uint32_t>(a + flags_at);
			const uint32_t expected = setup ? (pw == 8 ? 128 : 84) : extended ? (pw == 8 ? 136 : 84) : (pw == 8 ? 120 : 66);
			if (call.load<uint32_t>(a) != expected) throw uint32_t(87);
			if (extended) call.store<uint32_t>(a + (pw == 8 ? 132 : 80), 0);
			const auto old_mode = call.load_pointer(a + dm);
			const auto old_names = call.load_pointer(a + dn);
			auto selected = old_mode ? printer_name(call, old_mode, wide) : host_default_printer();
			if (old_names) selected = printer_name(call, old_names + call.load<uint16_t>(old_names+2) * (wide ? 2 : 1), wide);
			auto p = read_mode(call, selected, old_mode, wide);
			if (setup && (flags & 2)) {
				const double units = flags & 8 ? 2540.0/72 : 1000.0/72;
				p.margin_left = call.load<int32_t>(a+flags_at+28)/units;
				p.margin_top = call.load<int32_t>(a+flags_at+32)/units;
				p.margin_right = call.load<int32_t>(a+flags_at+36)/units;
				p.margin_bottom = call.load<int32_t>(a+flags_at+40)/units;
			}
			if (!(flags & 0x400) && !host_print_panel(p, setup)) { call.ret(extended ? 0 : 0); return; }
			const auto selected_copies = p.copies;
			const bool driver_copies = flags & 0x40000;
			if (!setup && !driver_copies) p.copies = 1;
			const auto mode = context.allocator.allocate(mode_size(wide));
			if (!mode) throw uint32_t(8);
			write_mode(call, mode, p, wide);
			const auto name = from_utf8(p.name);
			const auto driver = std::u16string(u"WINSPOOL"), port = std::u16string(u"PORTPROMPT:");
			const size_t unit = wide ? 2 : 1;
			const auto names = context.allocator.allocate(8 + (driver.size()+name.size()+port.size()+3)*unit);
			if (!names) { context.allocator.release(mode); throw uint32_t(8); }
			call.store<uint16_t>(names, uint16_t(8/unit));
			call.store<uint16_t>(names+2, uint16_t(8/unit+driver.size()+1));
			call.store<uint16_t>(names+4, uint16_t(8/unit+driver.size()+name.size()+2));
			call.store<uint16_t>(names+6, 1);
			size_t cursor=8;
			for (auto &s : {driver,name,port}) { if (wide) call.put_wide(names+cursor,s,s.size()+1); else call.put_narrow(names+cursor,to_ansi(s,0),s.size()+1); cursor+=(s.size()+1)*unit; }
			call.store_pointer(a+dm,mode); call.store_pointer(a+dn,names);
			if (old_mode) context.allocator.release(old_mode);
			if (old_names) context.allocator.release(old_names);
			if (!setup) {
				if (flags & (0x100 | 0x200)) call.store_pointer(a+hdc,make_dc(p));
				if (extended) {
					call.store<uint32_t>(a + (pw==8 ? 80 : 52), uint32_t(driver_copies ? 1 : selected_copies));
					const auto ranges = call.load_pointer(a + (pw==8 ? 64 : 40));
					const auto capacity = call.load<uint32_t>(a + (pw==8 ? 56 : 36));
					const bool ranged = p.first > 1 || p.last < 65535;
					if (ranged && ranges && capacity) {
						call.store<uint32_t>(ranges,uint32_t(p.first)); call.store<uint32_t>(ranges+4,uint32_t(p.last));
						call.store<uint32_t>(a + (pw==8 ? 52 : 32),1);
						call.store<uint32_t>(a+flags_at,(flags & ~1U) | 2U);
					} else call.store<uint32_t>(a + (pw==8 ? 52 : 32),0);
					call.store<uint32_t>(a + (pw==8 ? 132 : 80), 1); // PD_RESULT_PRINT
				} else {
					call.store<uint16_t>(a+flags_at+4,uint16_t(p.first)); call.store<uint16_t>(a+flags_at+6,uint16_t(p.last));
					call.store<uint16_t>(a+flags_at+12,uint16_t(driver_copies ? 1 : selected_copies));
					call.store<uint32_t>(a+flags_at,(flags & ~3U) | (p.first>1 || p.last<65535 ? 2U : 0U));
				}
			} else {
				const double units = flags & 8 ? 2540.0/72 : 1000.0/72;
				call.store<uint32_t>(a+flags_at,flags | 2U | (flags & 8 ? 8U : 4U));
				call.store<int32_t>(a+flags_at+4,int32_t(p.width*units)); call.store<int32_t>(a+flags_at+8,int32_t(p.height*units));
				const double minimum[4] = {p.left,p.top,p.width-p.right,p.height-p.bottom};
				const double margins[4] = {p.margin_left,p.margin_top,p.margin_right,p.margin_bottom};
				for (size_t n=0;n<4;++n) {
					call.store<int32_t>(a+flags_at+12+n*4,int32_t(minimum[n]*units));
					call.store<int32_t>(a+flags_at+28+n*4,int32_t(margins[n]*units));
				}
			}
			call.ret(extended ? 0 : 1);
		};
		registry.add(wide ? "PrintDlgW" : "PrintDlgA", 1, protect([=, this](ApiCall &call) { dialog(call,false,false); }));
		registry.add(wide ? "PrintDlgExW" : "PrintDlgExA", 1, protect([=, this](ApiCall &call) { dialog(call,true,false); }));
		registry.add(wide ? "PageSetupDlgW" : "PageSetupDlgA", 1, protect([=, this](ApiCall &call) { dialog(call,false,true); }));
	}
	registry.add("ClosePrinter", 1, [handles](ApiCall &call) { if (!handles->erase(call[0])) call.fail(6); else call.ret(1); });
	for (const auto *name : {"StartPage", "EndPage", "EndDoc", "AbortDoc"}) registry.add(name, 1, protect([=, this](ApiCall &call) {
		auto *dc = objects.dc(call[0]); if (!dc || !dc->printer) throw uint32_t(6);
		auto &p = *dc->printer;
		if (!p.document) throw uint32_t(3003);
		const std::string_view operation(name);
		if (operation == "StartPage") {
			if (p.page_open) throw uint32_t(87);
			auto pixels = objects.bitmap(dc->bitmap)->pixels(); std::fill(pixels.begin(),pixels.end(),255); p.page_open=true;
		} else if (operation == "EndPage") {
			if (!p.page_open) throw uint32_t(87);
			auto &b = *objects.bitmap(dc->bitmap);
			if (!p.document->page(b.pixels(),b.width,b.height)) throw uint32_t(29);
			p.page_open=false;
		} else if (operation == "EndDoc") {
			if (p.page_open) throw uint32_t(87);
			const bool ok = p.document->finish(); p.document.reset(); if (!ok) throw uint32_t(29);
		} else { p.document.reset(); p.page_open=false; }
		call.ret(1);
	}));
	registry.add("SetAbortProc", 2, [](ApiCall &call) { call.ret(1); });
#else
	(void)registry; (void)context;
#endif
}
}
