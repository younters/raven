#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* A frameless top-level window that draws its own caption: it answers WM_NCHITTEST with HTCAPTION over its top 20 pixels and with
   HTTRANSPARENT over its bottom-right corner. The host moves the pointer over the caption, presses and releases there, moves over the
   corner and then over the client area (frameless_hit_test.cpp). */

static int caption_moves, caption_presses, transparent_seen, client_misplaced;
static int result = 1;

static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	const int x = (short)LOWORD(lparam), y = (short)HIWORD(lparam);
	switch (message) {
	case WM_NCHITTEST:
		if (y < 20) return HTCAPTION;
		if (x >= 80 && y >= 80) return HTTRANSPARENT;
		return DefWindowProcW(window, message, wparam, lparam);
	case WM_NCMOUSEMOVE:
		if (x >= 80 && y >= 80) ++transparent_seen;
		else if (wparam == HTCAPTION && y < 20) ++caption_moves;
		return 0;
	case WM_NCLBUTTONDOWN:
		if (wparam == HTCAPTION && y < 20) ++caption_presses;
		return 0;
	case WM_NCLBUTTONUP:
		return 0;
	case WM_LBUTTONDOWN:
	case WM_LBUTTONUP:
		++client_misplaced;
		return 0;
	case WM_MOUSEMOVE:
		/* Client coordinates; the window is at 0,0. */
		if (x >= 80 && y >= 80) ++transparent_seen;
		else if (y < 20) ++client_misplaced;
		else if (x == 50 && y == 50) {
			result = !caption_moves ? 10 : !caption_presses ? 11 : transparent_seen ? 12 : client_misplaced ? 13 : 0;
			PostQuitMessage(0);
		}
		return 0;
	case WM_CLOSE:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

int raven_entry(void)
{
	WNDCLASSW definition = {0};
	MSG message;
	definition.lpfnWndProc = procedure;
	definition.hInstance = GetModuleHandleW(NULL);
	definition.lpszClassName = L"raven.frameless.hit";
	if (!RegisterClassW(&definition)) return 2;
	HWND window = CreateWindowExW(0, definition.lpszClassName, L"frameless", WS_POPUP | WS_VISIBLE, 0, 0, 100, 100, NULL, NULL,
		definition.hInstance, NULL);
	if (!window) return 3;
	while (GetMessageW(&message, NULL, 0, 0) > 0) DispatchMessageW(&message);
	DestroyWindow(window);
	return result;
}
