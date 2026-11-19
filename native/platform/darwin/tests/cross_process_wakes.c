/* Wakes between two processes on the macOS desktop. A named event set by one process must reach a thread of the other that waits for it
   at once, not when the waiting process's idle slice runs out; and a window of one process inside another process's window, whose thread
   sleeps in MsgWaitForMultipleObjectsEx as a browser's UI thread does, must receive the size its parent's process gives it. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

enum { ping, pong, ready, quit, sized, shared, handle_count };
static ULONG_PTR words[handle_count];

static unsigned hexadecimal(WCHAR *line, unsigned at, ULONG_PTR value)
{
	line[at++] = ' ';
	for (unsigned n = 0; n < sizeof(value) * 2; ++n) line[at++] = L"0123456789abcdef"[(value >> ((sizeof(value) * 2 - n - 1) * 4)) & 15];
	line[at] = 0;
	return at;
}

static double milliseconds(void)
{
	LARGE_INTEGER count, frequency;
	QueryPerformanceCounter(&count);
	QueryPerformanceFrequency(&frequency);
	return (double)count.QuadPart * 1000.0 / (double)frequency.QuadPart;
}

static LRESULT CALLBACK child_procedure(HWND window, UINT message, WPARAM w, LPARAM l)
{
	if (message == WM_SIZE) SetEvent((HANDLE)words[sized]);
	return DefWindowProcW(window, message, w, l);
}

static int child(HWND parent)
{
	WNDCLASSW kind = {0};
	kind.lpfnWndProc = child_procedure;
	kind.hInstance = GetModuleHandleW(NULL);
	kind.lpszClassName = L"RavenCrossProcessChild";
	if (!RegisterClassW(&kind)) return 30;
	HWND window = CreateWindowExW(0, kind.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, NULL, NULL, kind.hInstance, NULL);
	if (!window) return 31;
	/* A top-level window's previous parent is the desktop window, as Windows reports it. */
	if (SetParent(window, parent) != GetDesktopWindow()) return 32;
	HWND *slot = MapViewOfFile((HANDLE)words[shared], FILE_MAP_WRITE, 0, 0, sizeof(HWND));
	if (!slot) return 33;
	*slot = window;
	if (!SetEvent((HANDLE)words[ready])) return 34;
	/* The round trips: each ping is answered at once. The quit event ends them. */
	HANDLE round[2] = {(HANDLE)words[ping], (HANDLE)words[quit]};
	while (WaitForMultipleObjects(2, round, FALSE, 30000) == WAIT_OBJECT_0)
		if (!SetEvent((HANDLE)words[pong])) return 35;
	/* The sizes: the thread sleeps for messages and the parent's process places the window. */
	/* The sizes: the thread sleeps for messages and the parent's process places the window; a ping ends the test. */
	for (DWORD began = GetTickCount(); GetTickCount() - began < 30000;) {
		const DWORD woke = MsgWaitForMultipleObjectsEx(1, (HANDLE *)&words[ping], 30000, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
		if (woke == WAIT_OBJECT_0) return 0;
		if (woke != WAIT_OBJECT_0 + 1) return 36;
		MSG message;
		while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
	}
	return 37;
}

static int parent(void)
{
	SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
	for (unsigned n = 0; n < shared; ++n)
		words[n] = (ULONG_PTR)CreateEventW(&security, n == ready || n == quit, FALSE, NULL);
	words[shared] = (ULONG_PTR)CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, 4096, NULL);
	HWND window = CreateWindowExW(0, L"STATIC", L"cross-process wakes", WS_POPUP | WS_VISIBLE, 100, 100, 64, 64, NULL, NULL,
		GetModuleHandleW(NULL), NULL);
	for (unsigned n = 0; n < handle_count; ++n) if (!words[n]) return 3;
	if (!window) return 3;
	WCHAR line[1024];
	unsigned at = 0;
	line[at++] = '"'; at += GetModuleFileNameW(NULL, line + at, 900); line[at++] = '"'; line[at++] = ' '; line[at++] = '-'; line[at++] = 'c';
	line[at] = 0;
	at = hexadecimal(line, at, (ULONG_PTR)window);
	for (unsigned n = 0; n < handle_count; ++n) at = hexadecimal(line, at, words[n]);
	STARTUPINFOW startup = {0};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process = {0};
	if (!CreateProcessW(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process)) return 1;
	HANDLE starting[2] = {(HANDLE)words[ready], process.hProcess};
	if (WaitForMultipleObjects(2, starting, FALSE, 30000) != WAIT_OBJECT_0) return 4;
	HWND *slot = MapViewOfFile((HANDLE)words[shared], FILE_MAP_READ, 0, 0, sizeof(HWND));
	HWND inner = slot ? *slot : NULL;
	if (!inner) return 5;
	/* Before cross-process wakes reached a windowed process's idle wait, about a third of these took the whole 20 ms slice. */
	unsigned late = 0;
	for (unsigned n = 0; n < 50; ++n) {
		const double started = milliseconds();
		if (!SetEvent((HANDLE)words[ping]) || WaitForSingleObject((HANDLE)words[pong], 5000) != WAIT_OBJECT_0) return 20;
		if (milliseconds() - started > 15) ++late;
		Sleep(1);
	}
	if (late > 5) return 21;
	if (!SetEvent((HANDLE)words[quit])) return 6;
	Sleep(50);
	/* Before placements were announced to their window's process, none of these arrived while its thread slept. */
	ResetEvent((HANDLE)words[sized]);
	for (unsigned n = 0; n < 10; ++n) {
		if (!SetWindowPos(inner, NULL, 0, 0, 20 + (int)n * 3, 20 + (int)n * 2, SWP_NOZORDER | SWP_NOACTIVATE)) return 22;
		if (WaitForSingleObject((HANDLE)words[sized], 1000) != WAIT_OBJECT_0) return 23;
	}
	SetEvent((HANDLE)words[ping]);
	DWORD code = 99;
	if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0 || !GetExitCodeProcess(process.hProcess, &code)) return 2;
	DestroyWindow(window);
	return (int)code;
}

int raven_entry(void)
{
	const WCHAR *line = GetCommandLineW();
	for (unsigned i = 0; line[i]; ++i)
		if (line[i] == '-' && line[i + 1] == 'c' && line[i + 2] == ' ') {
			unsigned at = i + 2;
			ULONG_PTR values[handle_count + 1] = {0};
			for (unsigned n = 0; n <= handle_count; ++n) {
				while (line[at] == ' ') ++at;
				for (unsigned k = 0; k < sizeof(ULONG_PTR) * 2; ++k) {
					const WCHAR c = line[at++];
					values[n] = values[n] * 16 + (ULONG_PTR)(c <= '9' ? c - '0' : c - 'a' + 10);
				}
			}
			for (unsigned n = 0; n < handle_count; ++n) words[n] = values[n + 1];
			return child((HWND)values[0]);
		}
	return parent();
}
