#include <windows.h>

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	return DefWindowProcW(window, message, wparam, lparam);
}

/* Chromium copies its software compositor's DIB into a parent with
 * WS_CLIPCHILDREN while transparent input HWNDs cover the page. */
static int check_copy(HINSTANCE instance, int siblings)
{
	HWND root = CreateWindowExW(0, L"RavenTransparentClip", L"transparent clipping", WS_POPUP | WS_VISIBLE |
		(siblings ? 0 : WS_CLIPCHILDREN), 100, 100, 64, 48, NULL, NULL, instance, NULL);
	if (!root) return 1;
	HWND lower = siblings ? CreateWindowExW(0, L"RavenTransparentClip", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
		0, 0, 64, 48, root, NULL, instance, NULL) : root;
	HWND transparent = CreateWindowExW(WS_EX_TRANSPARENT, L"RavenTransparentClip", L"", WS_CHILD | WS_VISIBLE,
		8, 8, 24, 24, root, NULL, instance, NULL);
	HWND opaque = CreateWindowExW(0, L"RavenTransparentClip", L"", WS_CHILD | WS_VISIBLE,
		40, 8, 16, 24, root, NULL, instance, NULL);
	if (!lower || !transparent || !opaque) return 2;
	/* Put both siblings above the drawing window. */
	if (siblings && (!SetWindowPos(transparent, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) ||
		!SetWindowPos(opaque, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE))) return 3;
	HDC whole = GetWindowDC(root), destination = GetDC(lower), memory = CreateCompatibleDC(whole);
	BITMAPINFO info = {0};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = 64;
	info.bmiHeader.biHeight = -48;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	DWORD *bits = NULL;
	HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
	if (!whole || !destination || !memory || !bitmap || !bits) return 4;
	HGDIOBJ old = SelectObject(memory, bitmap);
	if (!old) return 5;
	/* Establish a known destination, then copy a nonuniform page over it. */
	for (unsigned i = 0; i < 64 * 48; ++i) bits[i] = 0x00102030;
	if (!BitBlt(whole, 0, 0, 64, 48, memory, 0, 0, SRCCOPY)) return 6;
	for (unsigned y = 0; y < 48; ++y)
		for (unsigned x = 0; x < 64; ++x) bits[y * 64 + x] = 0x00804020 + (y << 8) + x;
	if (!BitBlt(destination, 0, 0, 64, 48, memory, 0, 0, SRCCOPY) ||
		!BitBlt(memory, 0, 0, 64, 48, whole, 0, 0, SRCCOPY) || !GdiFlush()) return 7;
	for (unsigned y = 0; y < 48; ++y)
		for (unsigned x = 0; x < 64; ++x) {
			DWORD expected = x >= 40 && x < 56 && y >= 8 && y < 32 ? 0x00102030 : 0x00804020 + (y << 8) + x;
			if ((bits[y * 64 + x] & 0xffffff) != expected) return 8;
		}
	if (SelectObject(memory, old) != bitmap || !DeleteObject(bitmap) || !DeleteDC(memory) ||
		!ReleaseDC(lower, destination) || !ReleaseDC(root, whole) || !DestroyWindow(root)) return 9;
	return 0;
}

int raven_entry(void)
{
	HINSTANCE instance = GetModuleHandleW(NULL);
	WNDCLASSW wc = {0};
	wc.lpfnWndProc = window_proc;
	wc.hInstance = instance;
	wc.lpszClassName = L"RavenTransparentClip";
	if (!RegisterClassW(&wc)) return 1;
	int result = check_copy(instance, 0);
	if (result) return 10 + result;
	result = check_copy(instance, 1);
	if (result) return 20 + result;
	return UnregisterClassW(wc.lpszClassName, instance) ? 0 : 30;
}
