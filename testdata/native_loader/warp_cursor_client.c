#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Mouse look without raw input: the cursor is hidden, and each frame its movement is read and the cursor is put back. */
static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_NCCREATE) return TRUE;
	return DefWindowProcW(window, message, wparam, lparam);
}

/* Handles what the host sent until the cursor is where it should be, or a bounded wait ends. */
static int cursor_reaches(LONG x, LONG y)
{
	POINT position = {0, 0};
	for (int tries = 0; tries < 400; ++tries) {
		MSG message;
		while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
		if (!GetCursorPos(&position)) return 0;
		if (position.x == x && position.y == y) return 1;
		Sleep(5);
	}
	return 0;
}

int raven_entry(void)
{
	WNDCLASSW definition = {0};
	definition.hInstance = GetModuleHandleW(NULL);
	definition.lpfnWndProc = procedure;
	definition.lpszClassName = L"RavenWarpCursor";
	if (!RegisterClassW(&definition)) return 1;
	HWND window = CreateWindowExW(0, definition.lpszClassName, L"warp", WS_POPUP | WS_VISIBLE, 0, 0, 64, 64,
		NULL, NULL, definition.hInstance, NULL);
	if (!window || !ValidateRect(window, NULL)) return 2;
	if (ShowCursor(FALSE) >= 0) return 3;
	if (!SetWindowTextW(window, L"warp: ready")) return 4;
	/* The window gets the focus from the host before the pointer can be held. */
	for (int tries = 0; GetActiveWindow() != window; ++tries) {
		MSG message;
		if (tries > 400) return 5;
		while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
		Sleep(5);
	}
	if (!SetCursorPos(20, 20) || !SetWindowTextW(window, L"warp: held")) return 6;
	/* The host sends movement of +5,-3 and +7,+9, with the held pointer's own position after each. */
	if (!cursor_reaches(32, 26)) return 7;
	if (!SetCursorPos(20, 20) || !SetWindowTextW(window, L"warp: again")) return 8;
	/* Put back, the cursor moves from there: -4,+6. */
	if (!cursor_reaches(16, 26)) return 9;
	return 0;
}
