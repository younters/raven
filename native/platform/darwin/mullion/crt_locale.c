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
    char **(*command_line)(void) = (void *)GetProcAddress(crt, "__p__acmdln");
    if (!command_line || !*command_line() || lstrcmpA(*command_line(), GetCommandLineA())) ExitProcess(6);
    int *(*crt_errno)(void) = (void *)GetProcAddress(crt, "_errno");
    void *(*aligned)(size_t, size_t) = (void *)GetProcAddress(crt, "_aligned_malloc");
    void *(*offset)(size_t, size_t, size_t) = (void *)GetProcAddress(crt, "_aligned_offset_malloc");
    void *(*resize)(void *, size_t, size_t) = (void *)GetProcAddress(crt, "_aligned_realloc");
    void (*release)(void *) = (void *)GetProcAddress(crt, "_aligned_free");
    if (!crt_errno || !aligned || !offset || !resize || !release) ExitProcess(7);
    char guard[3] = {'x', 'x', 'x'};
    if (convert(&n, guard + 1, 1, 0xe9, NULL) || n != 1 || (unsigned char)guard[1] != 0xe9 || guard[0] != 'x' || guard[2] != 'x') ExitProcess(8);
    if (convert(&n, guard + 1, 1, 0xd800, NULL) != 42 || *crt_errno() != 42 || guard[2] != 'x') ExitProcess(9);
    if (aligned((size_t)-8, 64) || *crt_errno() != 12) ExitProcess(10);
    if (offset((size_t)-8, 64, 1) || *crt_errno() != 12) ExitProcess(11);
    char *block = offset(32, 64, 3);
    if (!block || ((ULONG_PTR)block + 3) % 64) ExitProcess(12);
    for (int i = 0; i < 32; ++i) block[i] = (char)i;
    block = resize(block, 128, 64);
    if (!block || (ULONG_PTR)block % 64) ExitProcess(13);
    for (int i = 0; i < 32; ++i) if (block[i] != i) ExitProcess(14);
    release(block);
    ExitProcess(0);
}
