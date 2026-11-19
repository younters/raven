#include "nonclient.h"

namespace raven {

DesktopMetrics DesktopMetrics::at_dpi(uint32_t value) const
{
	auto result=*this;
	for (auto *metric : {&result.border_x,&result.border_y,&result.dialog_x,&result.dialog_y,&result.frame_x,&result.frame_y,
		&result.caption,&result.small_caption,&result.menu,&result.button_x,&result.button_y,&result.small_button_x,&result.small_button_y})
		*metric=int32_t((int64_t(*metric)*value+dpi/2)/dpi);
	result.dpi=value;
	return result;
}

int32_t DesktopMetrics::get(int32_t index) const
{
	switch (index) {
	case 0: return screen_x;
	case 1: return screen_y;
	case 5: return border_x;
	case 6: return border_y;
	case 7: return dialog_x;
	case 8: return dialog_y;
	case 32: return frame_x;
	case 33: return frame_y;
	case 4: return caption;
	case 51: return small_caption;
	case 15: return menu;
	case 30: return button_x;
	case 31: return button_y;
	case 52: return small_button_x;
	case 53: return small_button_y;
	default: throw uint32_t(120);
	}
}

// How many border widths the frame around the client area takes: a raised edge is two, a static edge one, a window with any caption bit adds
// the line of its border, and a client edge two more. A minimized window's frame does not resize, so the sizing part is left out.
DesktopRect adjust_window_rect(const DesktopMetrics &metrics,DesktopRect rect,uint32_t style,bool menu,uint32_t ex_style)
{
	if (metrics.dpi!=96) {
		// ReactOS counts border units before multiplying by SM_CXBORDER. Scale the result once, not those units twice.
		const auto frame=adjust_window_rect(metrics.at_dpi(96),{0,0,0,0},style,menu,ex_style);
		const auto scale=[&](int32_t value) { return int32_t((int64_t(value)*metrics.dpi+(value<0 ? -48 : 48))/96); };
		return {rect.left+scale(frame.left),rect.top+scale(frame.top),rect.right+scale(frame.right),rect.bottom+scale(frame.bottom)};
	}
	constexpr uint32_t minimized=0x20000000,sizing=0x40000,dialog_frame=0x400000,caption_bits=0xc00000;
	constexpr uint32_t modal_frame=1,client_edge=0x200,tool_window=0x80,static_edge=0x20000;
	const bool raised=(style&minimized) || (ex_style&modal_frame) || (!(ex_style&static_edge) && (style&(sizing|dialog_frame)));
	int64_t lines=raised ? 2 : (ex_style&static_edge) ? 1 : 0;
	if (ex_style&client_edge)
		lines+=2;
	if ((style&caption_bits) || (ex_style&modal_frame))
		++lines;
	int64_t across=lines,down=lines;
	if ((style&sizing) && !(style&minimized)) {
		across+=int64_t(metrics.get(32))-metrics.get(7);
		down+=int64_t(metrics.get(33))-metrics.get(8);
	}
	across*=metrics.get(5);
	down*=metrics.get(6);
	// Wide intermediates avoid signed overflow; the public RECT is serialized modulo 32 bits.
	int64_t top=rect.top;
	if (menu)
		top-=metrics.get(15);
	if ((style&caption_bits)==caption_bits)
		top-=metrics.get((ex_style&tool_window) ? 51 : 4);
	return {int32_t(uint32_t(rect.left-across)),int32_t(uint32_t(top-down)),int32_t(uint32_t(rect.right+across)),int32_t(uint32_t(rect.bottom+down))};
}

}
