#pragma once

#include "desktop/objects.h"

namespace raven {

// AppKit owns the pointer over native chrome. Keep one balanced hide request, independent of pointer confinement.
class CocoaCursor {
	NSCursor *image = NSCursor.arrowCursor;
	uint64_t handle = UINT64_MAX;
	int count = 0;
	bool hidden = false, active = false;

	static NSCursor *symbol(NSString *name)
	{
		NSImage *image = [NSImage imageWithSystemSymbolName:name accessibilityDescription:nil];
		if (!image) return NSCursor.arrowCursor;
		image.size = NSMakeSize(20, 20);
		return [[NSCursor alloc] initWithImage:image hotSpot:NSMakePoint(2, 2)];
	}

	static NSCursor *stock(uint64_t handle)
	{
		switch (handle - desktop_stock_cursor + 32512) {
		case 32513: return NSCursor.IBeamCursor;
		case 32514: case 32650: return symbol(@"hourglass"); // WAIT / APPSTARTING
		case 32515: return NSCursor.crosshairCursor;
		case 32642: return [NSCursor frameResizeCursorFromPosition:NSCursorFrameResizePositionTopLeft inDirections:NSCursorFrameResizeDirectionsAll];
		case 32643: return [NSCursor frameResizeCursorFromPosition:NSCursorFrameResizePositionTopRight inDirections:NSCursorFrameResizeDirectionsAll];
		case 32644: return NSCursor.resizeLeftRightCursor;
		case 32645: return NSCursor.resizeUpDownCursor;
		case 32640: case 32646: return NSCursor.openHandCursor;
		case 32648: return NSCursor.operationNotAllowedCursor;
		case 32649: return NSCursor.pointingHandCursor;
		case 32651: return symbol(@"questionmark.circle");
		default: return NSCursor.arrowCursor;
		}
	}

	static NSCursor *custom(const DesktopIcon &icon, double scale)
	{
		const auto &source = icon.image;
		const auto &pixels = source.color;
		if (!pixels.width || !pixels.height || pixels.rgba.size() != size_t(pixels.width) * pixels.height * 4)
			return NSCursor.arrowCursor;
		NSBitmapImageRep *bitmap = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:pixels.width pixelsHigh:pixels.height
			bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace
			bitmapFormat:NSBitmapFormatAlphaNonpremultiplied bytesPerRow:pixels.width * 4 bitsPerPixel:32];
		if (!bitmap) return NSCursor.arrowCursor;
		std::memcpy(bitmap.bitmapData, pixels.rgba.data(), pixels.rgba.size());
		if (!source.has_alpha)
			for (size_t i = 0; i < size_t(pixels.width) * pixels.height; ++i) {
				const bool masked = i < source.and_mask.size() && source.and_mask[i];
				// NSCursor has alpha, but no destination XOR. Inverted monochrome pixels are approximated by opaque white.
				const bool invert = icon.monochrome && masked && i < source.xor_mask.size() && source.xor_mask[i];
				bitmap.bitmapData[i * 4 + 3] = masked && !invert ? 0 : 255;
			}
		NSImage *image = [[NSImage alloc] initWithSize:NSMakeSize(pixels.width / scale, pixels.height / scale)];
		bitmap.size = image.size;
		[image addRepresentation:bitmap];
		return [[NSCursor alloc] initWithImage:image hotSpot:NSMakePoint(std::min(icon.hotspot_x, pixels.width - 1) / scale,
			std::min(icon.hotspot_y, pixels.height - 1) / scale)];
	}

public:
	~CocoaCursor() { if (hidden) [NSCursor unhide]; }

	void set(uint64_t value, int show_count, const DesktopIcon *custom_image, double scale)
	{
		count = show_count;
		if (handle == value) return;
		handle = value;
		image = custom_image ? custom(*custom_image, scale) : stock(value);
	}

	void apply(bool client, NSCursor *edge = nil)
	{
		const bool hide = client && !edge && (!handle || count < 0);
		if (hide != hidden) {
			if (hide) [NSCursor hide]; else [NSCursor unhide];
			hidden = hide;
		}
		if (client || edge) [(edge ?: image) set];
		else if (active) [NSCursor.arrowCursor set];
		active = client || edge;
	}
};

}
