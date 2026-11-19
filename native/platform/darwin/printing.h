#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace raven {

struct PrinterSettings {
	std::string name = "Raven PDF";
	std::string output;
	// Paper and printable rectangle, in PostScript points; GDI uses 300 pixels per inch.
	double width = 612, height = 792, left = 0, top = 0, right = 612, bottom = 792;
	double margin_left = 36, margin_top = 36, margin_right = 36, margin_bottom = 36;
	int copies = 1, first = 1, last = 65535;
	bool landscape = false, color = true;
};

class PrintDocument {
public:
	virtual ~PrintDocument() = default;
	virtual bool page(std::span<const uint8_t> bgra, uint32_t width, uint32_t height) = 0;
	virtual bool finish() = 0;
};

struct PrinterDC {
	PrinterSettings settings;
	std::unique_ptr<PrintDocument> document;
	bool page_open = false;
};

#ifdef __APPLE__
std::vector<std::string> host_printers();
std::string host_default_printer();
PrinterSettings host_printer_settings(const std::string &name);
bool host_print_panel(PrinterSettings &settings, bool page_setup);
std::unique_ptr<PrintDocument> host_print_document(const PrinterSettings &settings, const std::string &title, const std::string &output);
#endif

}
