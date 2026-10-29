#include <windows.h>
#include <winternl.h>

// The NT file natives as programs call them directly, and as Chromium's sandbox finds them: by a binary search of ntdll's export names.
NTSTATUS NTAPI NtQueryAttributesFile(POBJECT_ATTRIBUTES, PVOID);
NTSTATUS NTAPI NtQueryFullAttributesFile(POBJECT_ATTRIBUTES, PVOID);
NTSTATUS NTAPI NtImpersonateAnonymousToken(HANDLE);
NTSTATUS NTAPI NtOpenSection(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) ++a, ++b;
    return (unsigned char)*a - (unsigned char)*b;
}

static int exported(const char *name)
{
    BYTE *base = (BYTE *)GetModuleHandleA("ntdll.dll");
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY entry = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    IMAGE_EXPORT_DIRECTORY *exports = (IMAGE_EXPORT_DIRECTORY *)(base + entry.VirtualAddress);
    DWORD *names = (DWORD *)(base + exports->AddressOfNames);
    DWORD low = 0, high = exports->NumberOfNames;
    while (low < high) {
        DWORD middle = (low + high) / 2;
        int order = same(name, (char *)base + names[middle]);
        if (!order) return 1;
        if (order < 0) high = middle; else low = middle + 1;
    }
    return 0;
}

static UNICODE_STRING text(const wchar_t *value)
{
    UNICODE_STRING out;
    USHORT length = 0;
    while (value[length]) ++length;
    out.Length = out.MaximumLength = length * 2;
    out.Buffer = (PWSTR)value;
    return out;
}

static NTSTATUS create(HANDLE *handle, HANDLE root, const wchar_t *name, ULONG access, ULONG disposition, ULONG options, ULONG_PTR *information)
{
    UNICODE_STRING counted = text(name);
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &counted, OBJ_CASE_INSENSITIVE, root, NULL);
    IO_STATUS_BLOCK io;
    io.Information = 99;
    NTSTATUS status = NtCreateFile(handle, access | SYNCHRONIZE, &attributes, &io, NULL, FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, disposition, options | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (information) *information = io.Information;
    return status;
}

static NTSTATUS query(const wchar_t *name, FILE_NETWORK_OPEN_INFORMATION *full, FILE_BASIC_INFORMATION *basic)
{
    UNICODE_STRING counted = text(name);
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &counted, OBJ_CASE_INSENSITIVE, NULL, NULL);
    return full ? NtQueryFullAttributesFile(&attributes, full) : NtQueryAttributesFile(&attributes, basic);
}

void entry(void)
{
    static const char *names[] = {"NtCreateFile", "NtOpenFile", "NtQueryAttributesFile", "NtQueryFullAttributesFile", "NtSetInformationFile",
        "NtImpersonateAnonymousToken", "NtOpenSection"};
    for (int i = 0; i < 7; ++i)
        if (!exported(names[i])) ExitProcess(10 + i);
    SetLastError(1234);
    HANDLE folder, file, other;
    ULONG_PTR information;
    if (create(&folder, NULL, L"\\??\\C:\\ntfile", FILE_LIST_DIRECTORY, FILE_CREATE, FILE_DIRECTORY_FILE, &information) || information != 2)
        ExitProcess(20);
    if (create(&file, folder, L"a.txt", GENERIC_WRITE, FILE_CREATE, FILE_NON_DIRECTORY_FILE, &information) || information != 2)
        ExitProcess(21);
    DWORD written;
    if (!WriteFile(file, "hello", 5, &written, NULL) || written != 5) ExitProcess(22);
    CloseHandle(file);
    if (create(&file, NULL, L"\\??\\C:\\ntfile\\a.txt", GENERIC_READ, FILE_CREATE, 0, NULL) != (NTSTATUS)0xc0000035) ExitProcess(23);
    if (create(&file, NULL, L"\\??\\C:\\ntfile\\a.txt", GENERIC_READ, FILE_OPEN, FILE_DIRECTORY_FILE, NULL) != (NTSTATUS)0xc0000103)
        ExitProcess(24);
    if (create(&file, NULL, L"\\??\\C:\\ntfile", GENERIC_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE, NULL) != (NTSTATUS)0xc00000ba)
        ExitProcess(25);
    if (create(&file, NULL, L"\\??\\C:\\ntfile\\missing.txt", GENERIC_READ, FILE_OPEN, 0, NULL) != (NTSTATUS)0xc0000034) ExitProcess(26);
    if (create(&file, NULL, L"\\??\\C:\\ntfile\\none\\x.txt", GENERIC_READ, FILE_OPEN, 0, NULL) != (NTSTATUS)0xc000003a) ExitProcess(27);
    if (create(&file, NULL, L"C:\\ntfile\\a.txt", GENERIC_READ, FILE_OPEN, 0, NULL) != (NTSTATUS)0xc000003b) ExitProcess(28);
    UNICODE_STRING counted = text(L"\\??\\C:\\ntfile\\a.txt");
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &counted, 0, NULL, NULL);
    IO_STATUS_BLOCK io;
    if (NtOpenFile(&file, GENERIC_READ | SYNCHRONIZE, &attributes, &io, FILE_SHARE_READ, FILE_SYNCHRONOUS_IO_NONALERT) || io.Information != 1)
        ExitProcess(29);
    char buffer[8] = {0};
    DWORD read;
    if (!ReadFile(file, buffer, sizeof buffer, &read, NULL) || read != 5 || buffer[0] != 'h' || buffer[4] != 'o') ExitProcess(30);
    CloseHandle(file);

    FILE_NETWORK_OPEN_INFORMATION full;
    FILE_BASIC_INFORMATION basic;
    if (query(L"\\??\\C:\\ntfile\\a.txt", &full, NULL) || full.EndOfFile.QuadPart != 5 || (full.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        !full.LastWriteTime.QuadPart || !full.ChangeTime.QuadPart)
        ExitProcess(31);
    if (query(L"\\??\\C:\\ntfile", NULL, &basic) || !(basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) ExitProcess(32);
    if (query(L"\\??\\C:\\ntfile\\missing.txt", NULL, &basic) != (NTSTATUS)0xc0000034) ExitProcess(33);

    if (create(&file, NULL, L"\\??\\C:\\ntfile\\a.txt", GENERIC_WRITE | DELETE, FILE_OPEN, 0, NULL)) ExitProcess(34);
    LARGE_INTEGER end;
    end.QuadPart = 2;
    if (NtSetInformationFile(file, &io, &end, sizeof end, 20 /* FileEndOfFileInformation */)) ExitProcess(35);
    struct {
        BOOLEAN replace;
        HANDLE root;
        ULONG length;
        WCHAR name[32];
    } rename = {TRUE, NULL, 0, L"\\??\\C:\\ntfile\\b.txt"};
    rename.length = 19 * 2;
    if (NtSetInformationFile(file, &io, &rename, sizeof rename, 10 /* FileRenameInformation */)) ExitProcess(36);
    CloseHandle(file);
    if (query(L"\\??\\C:\\ntfile\\a.txt", NULL, &basic) != (NTSTATUS)0xc0000034) ExitProcess(37);
    if (query(L"\\??\\C:\\ntfile\\b.txt", &full, NULL) || full.EndOfFile.QuadPart != 2) ExitProcess(38);
    if (create(&file, folder, L"b.txt", DELETE, FILE_OPEN, 0, NULL)) ExitProcess(39);
    BOOLEAN remove = TRUE;
    if (NtSetInformationFile(file, &io, &remove, sizeof remove, 13 /* FileDispositionInformation */)) ExitProcess(40);
    if (NtSetInformationFile(file, &io, &remove, sizeof remove, 1000) != (NTSTATUS)0xc0000003) ExitProcess(41);
    CloseHandle(file);
    if (query(L"\\??\\C:\\ntfile\\b.txt", NULL, &basic) != (NTSTATUS)0xc0000034) ExitProcess(42);
    CloseHandle(folder);
    if (GetLastError() != 1234) ExitProcess(43);

    // The sandbox's anonymous impersonation: the thread's token is ANONYMOUS LOGON until it reverts.
    if (NtImpersonateAnonymousToken(GetCurrentThread())) ExitProcess(45);
    HANDLE token;
    BYTE user[64];
    DWORD size;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token) ||
        !GetTokenInformation(token, TokenUser, user, sizeof user, &size)) ExitProcess(46);
    const SID *sid = (const SID *)((TOKEN_USER *)user)->User.Sid;
    if (sid->SubAuthorityCount != 1 || sid->IdentifierAuthority.Value[5] != 5 || sid->SubAuthority[0] != 7) ExitProcess(47);
    CloseHandle(token);
    if (!RevertToSelf() || OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) ExitProcess(48);

    // A section CreateFileMapping named, found again through the session's object namespace.
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, L"RavenNtSection");
    if (!mapping) ExitProcess(50);
    char *view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 0);
    view[0] = 'z';
    UNICODE_STRING section = text(L"\\Sessions\\1\\BaseNamedObjects\\RavenNtSection");
    InitializeObjectAttributes(&attributes, &section, 0, NULL, NULL);
    HANDLE opened;
    if (NtOpenSection(&opened, SECTION_MAP_READ, &attributes)) ExitProcess(51);
    const char *seen = MapViewOfFile(opened, FILE_MAP_READ, 0, 0, 0);
    if (!seen || seen[0] != 'z') ExitProcess(52);
    UNICODE_STRING absent = text(L"\\Sessions\\1\\BaseNamedObjects\\RavenNoSection");
    InitializeObjectAttributes(&attributes, &absent, 0, NULL, NULL);
    if (NtOpenSection(&opened, SECTION_MAP_READ, &attributes) != (NTSTATUS)0xc0000034) ExitProcess(53);

    HANDLE out = CreateFileA("C:\\nt_file.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    const char done[] = "OK: NT file natives exported, opened, queried, renamed, truncated and deleted; anonymous impersonation; sections opened by NT name";
    if (out == INVALID_HANDLE_VALUE || !WriteFile(out, done, sizeof done - 1, &written, NULL)) ExitProcess(44);
    CloseHandle(out);
    ExitProcess(0);
}
