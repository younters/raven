/* Import and edge-case checks for the C-locale libc++ entry points. */
#include <windows.h>
#include <stddef.h>
void entry(void)
{
    HMODULE crt = LoadLibraryA("ucrtbase.dll");
    int (*convert)(size_t *, char *, size_t, wchar_t, void *) =
        (void *)GetProcAddress(crt, "wcrtomb_s");
    int (*alpha)(unsigned short, void *) = (void *)GetProcAddress(crt, "_iswalpha_l");
    int (*collate)(const char *, const char *, void *) = (void *)GetProcAddress(crt, "_strcoll_l");
    if (!convert || !alpha || !collate) ExitProcess(1);
    size_t n = 0;
    char out = '?';
    if (convert(&n, &out, 1, L'A', NULL) || n != 1 || out != 'A') ExitProcess(2);
    if (convert(&n, &out, 0, L'A', NULL) != 34 || n != (size_t)-1) ExitProcess(3);
    if (convert(&n, &out, 1, 0x100, NULL) != 42 || out != 0) ExitProcess(4);
    if (!alpha('A', NULL) || alpha('1', NULL) || collate("a", "b", NULL) >= 0) ExitProcess(5);
    ExitProcess(0);
}
