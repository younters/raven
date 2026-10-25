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

struct Nonclient {
	const DesktopMetrics &metrics;
	using BOOL=int32_t;
	using DWORD=uint32_t;
	using VOID=void;
	// Wide intermediates avoid signed overflow; the public RECT is serialized modulo 32 bits.
	struct RECT { int64_t left,top,right,bottom; };
	struct SIZE { int64_t cx,cy; };
	using LPRECT=RECT *;
	static constexpr BOOL TRUE=1,FALSE=0;
	static constexpr DWORD WS_MINIMIZE=0x20000000,WS_THICKFRAME=0x40000,WS_CAPTION=0xc00000,WS_DLGFRAME=0x400000;
	static constexpr DWORD WS_EX_DLGMODALFRAME=1,WS_EX_STATICEDGE=0x20000,WS_EX_CLIENTEDGE=0x200,WS_EX_TOOLWINDOW=0x80;
	static constexpr int SM_CXFRAME=32,SM_CYFRAME=33,SM_CXDLGFRAME=7,SM_CYDLGFRAME=8,SM_CXBORDER=5,SM_CYBORDER=6;
	static constexpr int SM_CYMENU=15,SM_CYSMCAPTION=51,SM_CYCAPTION=4;
	int32_t GetSystemMetrics(int index) const { return metrics.get(index); }
	BOOL InflateRect(LPRECT rect,int64_t x,int64_t y)
	{
		rect->left-=x; rect->top-=y; rect->right+=x; rect->bottom+=y;
		return TRUE;
	}
#define WINAPI
#include "../upstream/reactos-nonclient/borders.inc"
#undef WINAPI
};

DesktopRect adjust_window_rect(const DesktopMetrics &metrics,DesktopRect rect,uint32_t style,bool menu,uint32_t ex_style)
{
	if (metrics.dpi!=96) {
		// ReactOS counts border units before multiplying by SM_CXBORDER. Scale the result once, not those units twice.
		const auto frame=adjust_window_rect(metrics.at_dpi(96),{0,0,0,0},style,menu,ex_style);
		const auto scale=[&](int32_t value) { return int32_t((int64_t(value)*metrics.dpi+(value<0 ? -48 : 48))/96); };
		return {rect.left+scale(frame.left),rect.top+scale(frame.top),rect.right+scale(frame.right),rect.bottom+scale(frame.bottom)};
	}
	Nonclient component{metrics};
	Nonclient::RECT result{rect.left,rect.top,rect.right,rect.bottom};
	component.RealAdjustWindowRectEx(&result,style,menu,ex_style);
	return {int32_t(uint32_t(result.left)),int32_t(uint32_t(result.top)),int32_t(uint32_t(result.right)),int32_t(uint32_t(result.bottom))};
}

}
