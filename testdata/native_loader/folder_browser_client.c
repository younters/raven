#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

static unsigned initialized;
static int CALLBACK browse(HWND window, UINT message, LPARAM selection, LPARAM data)
{
	if (message == BFFM_INITIALIZED) {
		++initialized;
		if (data == 1)
			SendMessageW(window, BFFM_SETSELECTIONW, TRUE, (LPARAM)L"C:\\Windows");
		else if (data == 2)
			SendMessageA(window, BFFM_SETSELECTIONA, TRUE, (LPARAM)"C:\\Windows");
		PostMessageW(window, WM_COMMAND, data ? IDOK : IDCANCEL, 0);
	}
	return 0;
}

int raven_entry(void)
{
	HMODULE shell = LoadLibraryW(L"shell32.dll");
	PIDLIST_ABSOLUTE (WINAPI *wide_browser)(LPBROWSEINFOW) = (void *)GetProcAddress(shell, "SHBrowseForFolderW");
	PIDLIST_ABSOLUTE (WINAPI *ansi_browser)(LPBROWSEINFOA) = (void *)GetProcAddress(shell, "SHBrowseForFolderA");
	void (WINAPI *free_list)(PIDLIST_RELATIVE) = (void *)GetProcAddress(shell, "ILFree");
	struct { BROWSEINFOW info; DWORD guard; } wide = {{0}, 0x12345678};
	struct { BROWSEINFOA info; DWORD guard; } ansi = {{0}, 0x87654321};
	WCHAR display[MAX_PATH + 1], path[MAX_PATH + 1];
	char display_a[MAX_PATH + 1], path_a[MAX_PATH + 1];
	if (!wide_browser || !ansi_browser || !free_list) return 1;
	if (wide_browser(NULL) || ansi_browser(NULL)) return 2;
	wide.info.lpfn = browse;
	wide.info.pszDisplayName = display;
	wide.info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
	wide.info.lParam = 1;
	display[MAX_PATH] = path[MAX_PATH] = 0x5a;
	PIDLIST_ABSOLUTE pidl = wide_browser(&wide.info);
	if (!pidl || initialized != 1 || !SHGetPathFromIDListW(pidl, path) || lstrcmpW(path, L"C:\\Windows")) return 3;
	if (lstrcmpW(display, L"Windows") || display[MAX_PATH] != 0x5a || path[MAX_PATH] != 0x5a || wide.guard != 0x12345678) return 4;
	if (!SHGetPathFromIDListA(pidl, path_a) || lstrcmpA(path_a, "C:\\Windows")) return 5;
	free_list(pidl);
	ansi.info.lpfn = browse;
	ansi.info.pszDisplayName = display_a;
	ansi.info.lParam = 2;
	display_a[MAX_PATH] = path_a[MAX_PATH] = 0x5a;
	pidl = ansi_browser(&ansi.info);
	if (!pidl || initialized != 2 || !SHGetPathFromIDListA(pidl, path_a) || lstrcmpA(path_a, "C:\\Windows")) return 6;
	if (lstrcmpA(display_a, "Windows") || display_a[MAX_PATH] != 0x5a || path_a[MAX_PATH] != 0x5a || ansi.guard != 0x87654321) return 7;
	CoTaskMemFree(pidl);
	wide.info.lParam = 0;
	if (wide_browser(&wide.info) || initialized != 3) return 8;
	if (!SHGetSpecialFolderPathW(NULL, path, CSIDL_PROGRAM_FILES, TRUE) || !path[0]) return 9;
	if (SHGetSpecialFolderLocation(NULL, CSIDL_PROGRAM_FILES, &pidl) || !pidl || !SHGetPathFromIDListW(pidl, display) || lstrcmpW(path, display)) return 10;
	free_list(pidl);
	free_list(NULL);
	return 0;
}
