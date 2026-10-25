#define _WIN32_WINNT 0x0a00
#define UNICODE
#include <windows.h>

static HWND child;
static unsigned moves, cursor_messages, captured_messages;
static BOOL parent_override;
static int failure;
static unsigned dpi_messages;
static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_DPICHANGED) {
        if (LOWORD(wp) != 192 || HIWORD(wp) != 192) failure = 23;
        ++dpi_messages;
        return 0;
    }
    if (message == WM_SETCURSOR) {
        ++cursor_messages;
        if (window != child && (HWND)wp == child && parent_override) {
            SetCursor(LoadCursorW(NULL, IDC_HAND));
            return TRUE;
        }
    }
    if (window == child && message == WM_MOUSEMOVE) {
        ++moves;
        if (moves == 1) {
            if (GetCursor() != LoadCursorW(NULL, IDC_IBEAM)) failure = 20;
            parent_override = TRUE;
        } else if (moves == 2) {
            if (GetCursor() != LoadCursorW(NULL, IDC_HAND)) failure = 21;
            SetCapture(child);
            captured_messages = cursor_messages;
        } else {
            if (cursor_messages != captured_messages || GetCursor() != LoadCursorW(NULL, IDC_HAND)) failure = 22;
            ReleaseCapture();
            PostQuitMessage(failure);
        }
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

void mainCRTStartup(void)
{
    typedef UINT (WINAPI *dpi_fn)(HWND);
    dpi_fn get_dpi = (dpi_fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    typedef BOOL (WINAPI *params_fn)(UINT,UINT,PVOID,UINT,UINT);
    params_fn parameters = (params_fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi");
    if (!get_dpi || !parameters) ExitProcess(1);
#ifdef PER_MONITOR_MANIFEST
    if (!IsProcessDPIAware()) ExitProcess(1);
#else
    if (IsProcessDPIAware()) ExitProcess(1);
#ifdef AWARE
    if (!SetProcessDPIAware() || !IsProcessDPIAware()) ExitProcess(2);
#endif
#endif
#ifdef AWARE
    const UINT dpi = 192;
#else
    const UINT dpi = 96;
#endif
    BYTE and_bits[4] = {0,0,0x80,0}, xor_bits[4] = {0x80,0,0,0};
    HCURSOR custom = CreateCursor(NULL,1,1,2,2,and_bits,xor_bits);
    if (!custom) ExitProcess(3);
    SetCursor(custom);
    if (GetCursor() != custom || ShowCursor(FALSE) != -1 || ShowCursor(FALSE) != -2 || ShowCursor(TRUE) != -1 || ShowCursor(TRUE) != 0) ExitProcess(4);
    SetCursor(NULL);
    SetCursor(LoadCursorW(NULL, IDC_ARROW));
    DestroyCursor(custom);
    HINSTANCE instance = GetModuleHandleW(NULL);
    HCURSOR resource = LoadCursorW(instance, MAKEINTRESOURCEW(111));
    if (!resource || LoadCursorW(instance,MAKEINTRESOURCEW(111)) != resource) ExitProcess(13);
    SetCursor(resource);
    SetCursor(LoadCursorW(NULL,IDC_ARROW));
    HCURSOR sized = (HCURSOR)LoadImageW(instance,MAKEINTRESOURCEW(111),IMAGE_CURSOR,2,2,0);
    if (!sized) ExitProcess(14);
    SetCursor(sized);
    SetCursor(LoadCursorW(NULL,IDC_ARROW)); DestroyCursor(sized);
    HBITMAP bitmap = (HBITMAP)LoadImageW(instance,MAKEINTRESOURCEW(112),IMAGE_BITMAP,6,4,0);
    BITMAP info;
    if (!bitmap || !GetObjectW(bitmap,sizeof(info),&info) || info.bmWidth != 6 || info.bmHeight != 4) ExitProcess(15);
    DeleteObject(bitmap);
    BYTE mono_bits[8] = {0,0,128,0,128,0,0,0};
    ICONINFO ii = {0}; ii.xHotspot = ii.yHotspot = 1;
    ii.hbmMask = CreateBitmap(2,4,1,1,mono_bits);
    HCURSOR indirect = CreateIconIndirect(&ii);
    if (!indirect) ExitProcess(16);
    SetCursor(indirect); SetCursor(LoadCursorW(NULL,IDC_ARROW));
    DestroyCursor(indirect); DeleteObject(ii.hbmMask);
    WNDCLASSW parent_class = {0};
    parent_class.lpfnWndProc = procedure;
    parent_class.lpszClassName = L"cursor parent";
    parent_class.hCursor = LoadCursorW(NULL, IDC_ARROW);
    WNDCLASSW child_class = parent_class;
    child_class.lpszClassName = L"cursor child";
    child_class.hCursor = LoadCursorW(NULL, IDC_IBEAM);
    if (!RegisterClassW(&parent_class) || !RegisterClassW(&child_class)) ExitProcess(5);
    HWND parent = CreateWindowExW(0,parent_class.lpszClassName,L"cursor DPI test",WS_OVERLAPPEDWINDOW,0,0,400,300,NULL,NULL,NULL,NULL);
    child = CreateWindowExW(0,child_class.lpszClassName,L"",WS_CHILD | WS_VISIBLE,10,10,100,100,parent,NULL,NULL,NULL);
    if (!parent || !child || get_dpi(child) != dpi || get_dpi(NULL) != 0) ExitProcess(6);
#ifdef PER_MONITOR_MANIFEST
    typedef HANDLE (WINAPI *get_context_fn)(HWND);
    get_context_fn get_context = (get_context_fn)GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetWindowDpiAwarenessContext");
    if (!get_context || get_context(parent) != (HANDLE)(LONG_PTR)-4) ExitProcess(19);
#endif
    HDC dc = GetDC(parent);
    HDC memory = CreateCompatibleDC(dc);
    if (GetDeviceCaps(dc,LOGPIXELSX) != (int)dpi || GetDeviceCaps(memory,LOGPIXELSY) != (int)dpi) ExitProcess(7);
    DeleteDC(memory); ReleaseDC(parent,dc);
    NONCLIENTMETRICSW metrics = {0}; metrics.cbSize = sizeof(metrics);
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0) || metrics.lfMessageFont.lfHeight != -(int)(dpi/8)) ExitProcess(8);
    if (!parameters(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0,144) || metrics.lfMessageFont.lfHeight != -18) ExitProcess(9);
    typedef HANDLE (WINAPI *context_fn)(HANDLE);
    context_fn set_context = (context_fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext");
    if (!set_context) ExitProcess(11);
    HANDLE previous = set_context((HANDLE)(LONG_PTR)-1);
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0) || metrics.lfMessageFont.lfHeight != -12) ExitProcess(12);
    set_context(previous);
    LOGFONTW gui_font;
    if (!GetObjectW(GetStockObject(DEFAULT_GUI_FONT),sizeof(gui_font),&gui_font) || gui_font.lfHeight != -(int)(11*dpi/96)) ExitProcess(17);
    RECT rect = {0,0,100,100};
    if (!AdjustWindowRectExForDpi(&rect,WS_OVERLAPPEDWINDOW,FALSE,0,96) || rect.left != -4 || rect.top != -30) ExitProcess(10);
    ShowWindow(parent,SW_SHOW);
    GetWindowRect(parent,&rect);
    if (rect.right-rect.left != (int)(400*dpi/96) || rect.bottom-rect.top != (int)(300*dpi/96) || dpi_messages != (dpi == 192)) ExitProcess(18);
    WINDOWPLACEMENT placement = {0}; placement.length = sizeof(placement);
    if (!GetWindowPlacement(parent,&placement) || placement.rcNormalPosition.right-placement.rcNormalPosition.left != 400) ExitProcess(24);
    if (!SetWindowPlacement(parent,&placement)) ExitProcess(25);
    GetWindowRect(parent,&rect);
    if (rect.right-rect.left != (int)(400*dpi/96) || dpi_messages != (dpi == 192)) ExitProcess(26);
    MSG message;
    while (GetMessageW(&message,NULL,0,0) > 0) DispatchMessageW(&message);
    ExitProcess((UINT)message.wParam);
}
