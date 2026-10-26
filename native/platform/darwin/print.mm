#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include "desktop/printing.h"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <unistd.h>

namespace raven {
namespace {
NSString *string(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()]; }
std::string text(NSString *s) { return s ? std::string(s.UTF8String) : std::string(); }
NSPrintInfo *info(const PrinterSettings &p)
{
	NSPrintInfo *i = [[NSPrintInfo sharedPrintInfo] copy];
	if (p.name != "Raven PDF") i.printer = [NSPrinter printerWithName:string(p.name)];
	i.orientation = p.landscape ? NSPaperOrientationLandscape : NSPaperOrientationPortrait;
	i.paperSize = NSMakeSize(p.width, p.height);
	i.leftMargin = p.margin_left; i.topMargin = p.margin_top;
	i.rightMargin = p.margin_right; i.bottomMargin = p.margin_bottom;
	i.dictionary[NSPrintCopies] = @(p.copies);
	i.dictionary[NSPrintFirstPage] = @(p.first);
	i.dictionary[NSPrintLastPage] = @(p.last);
	return i;
}
void read_info(PrinterSettings &p, NSPrintInfo *i)
{
	p.width = i.paperSize.width; p.height = i.paperSize.height;
	p.landscape = i.orientation == NSPaperOrientationLandscape;
	NSRect r = i.imageablePageBounds;
	p.left = r.origin.x; p.top = p.height - NSMaxY(r);
	p.right = NSMaxX(r); p.bottom = p.height - r.origin.y;
	p.margin_left = i.leftMargin; p.margin_top = i.topMargin;
	p.margin_right = i.rightMargin; p.margin_bottom = i.bottomMargin;
	p.copies = std::max(1, [i.dictionary[NSPrintCopies] intValue]);
	p.first = std::max(1, [i.dictionary[NSPrintFirstPage] intValue]);
	int last = [i.dictionary[NSPrintLastPage] intValue];
	p.last = last > 0 ? std::min(last,65535) : 65535;
}
class MacDocument final : public PrintDocument {
	PrinterSettings settings;
	std::string path, destination;
	bool spool = false;
	CGContextRef pdf = nullptr;
	bool completed = false;
	unsigned pages = 0;
public:
	MacDocument(const PrinterSettings &p, const std::string &title, const std::string &output) : settings(p)
	{
		static std::atomic<unsigned> serial{0};
		spool = p.name != "Raven PDF" && output.empty() && p.output.empty();
		if (!output.empty()) path = output;
		else if (!p.output.empty()) path = p.output;
		else if (p.name == "Raven PDF") {
			NSString *downloads = NSSearchPathForDirectoriesInDomains(NSDownloadsDirectory, NSUserDomainMask, YES).firstObject;
			std::filesystem::create_directories(text(downloads));
			path = text(downloads) + "/Raven-" + std::to_string(getpid()) + "-" + std::to_string(++serial) + ".pdf";
		} else path = text(NSTemporaryDirectory()) + "raven-print-" + std::to_string(getpid()) + "-" + std::to_string(++serial) + ".pdf";
		destination = path;
		// Stage every job in a private file: AbortDoc must not delete an existing output file.
		std::string staging = path + ".XXXXXX";
		std::vector<char> buffer(staging.begin(), staging.end()); buffer.push_back(0);
		int fd = mkstemp(buffer.data());
		if (fd < 0) { path.clear(); return; }
		close(fd); path = buffer.data();
		NSURL *url = [NSURL fileURLWithPath:string(path)];
		CGRect box = CGRectMake(0, 0, p.width, p.height);
		NSDictionary *metadata = @{(id)kCGPDFContextTitle: string(title), (id)kCGPDFContextCreator: @"Raven"};
		pdf = CGPDFContextCreateWithURL((__bridge CFURLRef)url, &box, (__bridge CFDictionaryRef)metadata);
	}
	~MacDocument() override
	{
		if (pdf) { CGPDFContextClose(pdf); CGContextRelease(pdf); }
		if (!completed && !path.empty()) unlink(path.c_str());
	}
	bool page(std::span<const uint8_t> bgra, uint32_t width, uint32_t height) override
	{
		if (!pdf || bgra.size() != size_t(width) * height * 4) return false;
		std::vector<uint8_t> gray;
		if (!settings.color) {
			gray.assign(bgra.begin(), bgra.end());
			for (size_t at=0; at<gray.size(); at+=4) {
				const uint8_t value = uint8_t((gray[at]*11 + gray[at+1]*59 + gray[at+2]*30)/100);
				gray[at] = gray[at+1] = gray[at+2] = value;
			}
			bgra = gray;
		}
		CGColorSpaceRef colors = CGColorSpaceCreateDeviceRGB();
		CGDataProviderRef data = CGDataProviderCreateWithData(nullptr, bgra.data(), bgra.size(), nullptr);
		CGImageRef image = CGImageCreate(width, height, 8, 32, size_t(width) * 4, colors,
			kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst, data, nullptr, false, kCGRenderingIntentDefault);
		if (!image) { CGDataProviderRelease(data); CGColorSpaceRelease(colors); return false; }
		CGPDFContextBeginPage(pdf, nullptr);
		// CGImage rows run from the top; the PDF rectangle is measured from the bottom.
		CGContextDrawImage(pdf, CGRectMake(settings.left, settings.height - settings.bottom,
			settings.right - settings.left, settings.bottom - settings.top), image);
		CGPDFContextEndPage(pdf);
		CGImageRelease(image); CGDataProviderRelease(data); CGColorSpaceRelease(colors);
		++pages;
		return true;
	}
	bool finish() override
	{
		if (!pdf || !pages) return false;
		CGPDFContextClose(pdf); CGContextRelease(pdf); pdf = nullptr;
		if (spool) {
			NSPrintInfo *i = info(settings);
			// The Windows caller already selected pages using the dialog's range.
			i.dictionary[NSPrintAllPages] = @YES;
			i.dictionary[NSPrintFirstPage] = @1;
			i.dictionary[NSPrintLastPage] = @(pages);
			CFArrayRef printers = nullptr;
			if (PMServerCreatePrinterList(kPMServerLocal, &printers) != noErr) return false;
			PMPrinter printer = nullptr;
			for (CFIndex n = 0; n < CFArrayGetCount(printers); ++n) {
				auto candidate = (PMPrinter)CFArrayGetValueAtIndex(printers, n);
				if (CFEqual(PMPrinterGetName(candidate), (__bridge CFStringRef)string(settings.name))) printer = candidate;
			}
			if (!printer) { CFRelease(printers); return false; }
			NSURL *url = [NSURL fileURLWithPath:string(path)];
			OSStatus status = PMPrinterPrintWithFile(printer, (PMPrintSettings)[i PMPrintSettings], (PMPageFormat)[i PMPageFormat], kPMDocumentFormatPDF, (__bridge CFURLRef)url);
			CFRelease(printers);
			if (status != noErr) return false;
			unlink(path.c_str());
		}
		if (!spool && rename(path.c_str(), destination.c_str()) != 0) return false;
		completed = true;
		return true;
	}
};
}
std::vector<std::string> host_printers()
{
	std::vector<std::string> result{"Raven PDF"};
	if (!std::getenv("RAVEN_PRINT_PDF_ONLY"))
		for (NSString *name in NSPrinter.printerNames) result.push_back(text(name));
	return result;
}
std::string host_default_printer()
{
	if (std::getenv("RAVEN_PRINT_PDF_ONLY")) return "Raven PDF";
	const auto name = text([NSPrintInfo sharedPrintInfo].printer.name);
	return name.empty() ? "Raven PDF" : name;
}
PrinterSettings host_printer_settings(const std::string &name)
{
	PrinterSettings p; p.name = name;
	if (name != "Raven PDF") {
		NSPrintInfo *i = [[NSPrintInfo sharedPrintInfo] copy];
		i.printer = [NSPrinter printerWithName:string(name)];
		read_info(p, i);
	}
	else {
		NSSize size = [NSPrintInfo sharedPrintInfo].paperSize;
		if (size.width > 0 && size.height > 0) { p.width = p.right = size.width; p.height = p.bottom = size.height; }
	}
	return p;
}
bool host_print_panel(PrinterSettings &p, bool page_setup)
{
	@autoreleasepool {
		// Host API dispatch is synchronous on the AppKit thread. A small native chooser also exposes the virtual queue,
		// which NSPrintPanel cannot list because it is not an installed macOS printer.
		if (!page_setup) {
			NSAlert *chooser = [NSAlert new]; chooser.messageText = @"Print";
			[chooser addButtonWithTitle:@"Continue"]; [chooser addButtonWithTitle:@"Cancel"];
			NSView *options = [[NSView alloc] initWithFrame:NSMakeRect(0,0,320,158)];
			NSPopUpButton *printers = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0,128,320,26) pullsDown:NO];
			for (const auto &name : host_printers()) [printers addItemWithTitle:string(name)];
			[printers selectItemWithTitle:string(p.name)]; [options addSubview:printers];
			auto field = [&](NSString *label, double x, double y, int value) {
				NSTextField *caption = [NSTextField labelWithString:label]; caption.frame = NSMakeRect(x,y+3,80,22); [options addSubview:caption];
				NSTextField *entry = [[NSTextField alloc] initWithFrame:NSMakeRect(x+82,y,64,24)]; entry.integerValue=value; [options addSubview:entry]; return entry;
			};
			NSTextField *copies = field(@"Copies",0,94,p.copies);
			NSTextField *first = field(@"From page",0,60,p.first);
			NSTextField *last = field(@"To page",165,60,p.last);
			NSButton *landscape = [NSButton checkboxWithTitle:@"Landscape" target:nil action:nil]; landscape.frame=NSMakeRect(0,22,150,24); landscape.state=p.landscape?NSControlStateValueOn:NSControlStateValueOff; [options addSubview:landscape];
			NSButton *mono = [NSButton checkboxWithTitle:@"Grayscale" target:nil action:nil]; mono.frame=NSMakeRect(165,22,150,24); mono.state=p.color?NSControlStateValueOff:NSControlStateValueOn; [options addSubview:mono];
			chooser.accessoryView=options;
			if ([chooser runModal] != NSAlertFirstButtonReturn) return false;
			const auto selected = text(printers.titleOfSelectedItem);
			if (selected != p.name) p = host_printer_settings(selected);
			p.copies=std::clamp<int>(int(copies.integerValue),1,999);
			p.first=std::clamp<int>(int(first.integerValue),1,65535);
			p.last=std::clamp<int>(int(last.integerValue),p.first,65535);
			const bool rotated=landscape.state==NSControlStateValueOn;
			if (rotated != p.landscape) {
				std::swap(p.width,p.height); std::swap(p.left,p.top);
				p.right=p.width-p.left; p.bottom=p.height-p.top;
			}
			p.landscape=rotated; p.color=mono.state!=NSControlStateValueOn;
			if (p.name == "Raven PDF") return true;
		}
		NSPrintInfo *i = info(p);
		NSInteger result = page_setup ? [[NSPageLayout pageLayout] runModalWithPrintInfo:i] : [[NSPrintPanel printPanel] runModalWithPrintInfo:i];
		if (result != NSModalResponseOK) return false;
		if (p.name != "Raven PDF") p.name = text(i.printer.name);
		read_info(p, i);
		if ([i.dictionary[NSPrintJobDisposition] isEqual:NSPrintSaveJob]) {
			NSURL *url = i.dictionary[NSPrintJobSavingURL];
			if (!url) return false;
			p.output = text(url.path);
		}
		return true;
	}
}
std::unique_ptr<PrintDocument> host_print_document(const PrinterSettings &p, const std::string &title, const std::string &output)
{
	if (std::getenv("RAVEN_PRINT_PDF_ONLY") && p.name != "Raven PDF") throw uint32_t(5);
	return std::make_unique<MacDocument>(p, title, output);
}
}
