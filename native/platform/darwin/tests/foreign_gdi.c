#include <windows.h>
#include <stdint.h>


unsigned int raven_entry(void)
{
    const WCHAR *command = GetCommandLineW(), *number = command;
    for (const WCHAR *p = command; *p; ++p) if (*p == ' ') number = p + 1;
    uintptr_t target_window = 0;
    while (*number >= '0' && *number <= '9') target_window = target_window * 10 + *number++ - '0';
    if (!target_window) return 6;
    HDC source = CreateCompatibleDC(NULL);
    if (!source) return 1;
    for (unsigned frame = 0; frame < 2; ++frame) {
        const unsigned width = frame ? 80 : 64, height = frame ? 40 : 48;
        BITMAPINFO info = {0};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -(LONG)height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        uint32_t *pixels = NULL;
        HBITMAP bitmap = CreateDIBSection(source, &info, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
        if (!bitmap || !pixels) return 2;
        HGDIOBJ previous = SelectObject(source, bitmap);
        for (unsigned i = 0; i < width * height; ++i)
            pixels[i] = frame ? 0xffff2000 : 0xff4080bf;
        HWND window = (HWND)(uintptr_t)target_window;
        HDC destination = GetDC(window);
        if (!destination) return 3;
        if (!BitBlt(destination, 0, 0, width, height, source, 0, 0, SRCCOPY)) return 4;
        if (!ReleaseDC(window, destination)) return 5;
        RECT rect;
        while (GetClientRect(window, &rect) && (unsigned)rect.right == width) Sleep(1);
        SelectObject(source, previous);
        DeleteObject(bitmap);
    }
    DeleteDC(source);
    return 0;
}
