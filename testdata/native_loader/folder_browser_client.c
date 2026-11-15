/* The classic folder browser, wide and ANSI: its callback hears BFFM_INITIALIZED, moves the selection with BFFM_SETSELECTION and
 * accepts or cancels, and the answer is a list the task allocator owns, with the folder's display name. Returns 0, or the number of the
 * first failing check. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

static unsigned initialized;

/* lParam of BROWSEINFO picks what the callback does: 1 selects C:\Windows by wide path, 2 by ANSI path, 0 cancels. */
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
	PIDLIST_ABSOLUTE list;
	if (!wide_browser || !ansi_browser || !free_list)
		return 1;
	if (wide_browser(NULL) || ansi_browser(NULL))
		return 2;
	/* Wide: the folder the callback selected, its name in a MAX_PATH buffer and nothing written past it. */
	wide.info.lpfn = browse;
	wide.info.pszDisplayName = display;
	wide.info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
	wide.info.lParam = 1;
	display[MAX_PATH] = path[MAX_PATH] = 0x5a;
	list = wide_browser(&wide.info);
	if (!list || initialized != 1 || !SHGetPathFromIDListW(list, path) || lstrcmpiW(path, L"C:\\Windows"))
		return 3;
	if (lstrcmpiW(display, L"Windows") || display[MAX_PATH] != 0x5a || path[MAX_PATH] != 0x5a || wide.guard != 0x12345678)
		return 4;
	/* The same list read in the ANSI code page. */
	if (!SHGetPathFromIDListA(list, path_a) || lstrcmpiA(path_a, "C:\\Windows"))
		return 5;
	free_list((PIDLIST_RELATIVE)list);
	/* ANSI: the same through the A forms; the list is freed with CoTaskMemFree, as SHBrowseForFolder's documentation says. */
	ansi.info.lpfn = browse;
	ansi.info.pszDisplayName = display_a;
	ansi.info.lParam = 2;
	display_a[MAX_PATH] = path_a[MAX_PATH] = 0x5a;
	list = ansi_browser(&ansi.info);
	if (!list || initialized != 2 || !SHGetPathFromIDListA(list, path_a) || lstrcmpiA(path_a, "C:\\Windows"))
		return 6;
	if (lstrcmpiA(display_a, "Windows") || display_a[MAX_PATH] != 0x5a || path_a[MAX_PATH] != 0x5a || ansi.guard != 0x87654321)
		return 7;
	CoTaskMemFree(list);
	/* Cancelled: no list. */
	wide.info.lParam = 0;
	if (wide_browser(&wide.info) || initialized != 3)
		return 8;
	return 0;
}
