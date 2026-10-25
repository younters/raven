#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <propsys.h>
#include <shobjidl.h>

/* No user32 or ole32 imports: packed programs discover these APIs after startup. */
static HMODULE user, ole;
#define API(name) ((__typeof__(&name))GetProcAddress(user, #name))
static unsigned initialized;
static BOOL modal;

static void check(BOOL value, UINT error)
{
	if (!value) ExitProcess(error);
}

static INT_PTR CALLBACK dialog_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_INITDIALOG) {
		check(lparam == (LPARAM)-123, 10);
		++initialized;
		if (modal) check(API(PostMessageW)(dialog, WM_COMMAND, IDOK, 0), 11);
		return TRUE;
	}
	if (message == WM_COMMAND && wparam == IDOK) {
		check(API(EndDialog)(dialog, -234), 12);
		/* EndDialog must return to the callback before the modal loop unwinds. */
		return TRUE;
	}
	return FALSE;
}

UINT raven_entry(void)
{
	HINSTANCE instance = GetModuleHandleW(NULL);
	HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(5));
	const DLGTEMPLATE *definition = LockResource(LoadResource(instance, resource));
	HWND dialog, item;
	WCHAR wide[32];
	char ansi[32];
	BOOL translated;
	RECT rect = {0, 0, 4, 8};
	MSG message = {0};
	IMalloc *allocator = NULL;
	void *memory;
	HRESULT (WINAPI *get_malloc)(DWORD, IMalloc **);
	HRESULT (WINAPI *get_store)(PCWSTR, IBindCtx *, GETPROPERTYSTOREFLAGS, REFIID, void **);
	HMODULE shell;
	void *store = (void *)1;
	const GUID property_store = {0x886d8eeb, 0x8cf2, 0x4446, {0x8d, 0x02, 0xcd, 0xba, 0x1d, 0xbd, 0xcf, 0x99}};

	user = LoadLibraryA("user32.dll");
	ole = LoadLibraryA("ole32.dll");
	check(user && ole && definition, 1);
	get_malloc = (void *)GetProcAddress(ole, "CoGetMalloc");
	check(get_malloc && get_malloc(MEMCTX_TASK, &allocator) == S_OK && allocator, 2);
	memory = IMalloc_Alloc(allocator, 32);
	check(memory != NULL, 3);
	((BYTE *)memory)[0] = 42;
	memory = IMalloc_Realloc(allocator, memory, 64);
	check(memory && ((BYTE *)memory)[0] == 42, 4);
	IMalloc_Free(allocator, memory);
	IMalloc_Release(allocator);
	shell = LoadLibraryA("shell32.dll");
	get_store = (void *)GetProcAddress(shell, "SHGetPropertyStoreFromParsingName");
	check(get_store && get_store(L"C:\\unsupported.lnk", NULL, GPS_READWRITE, &property_store, &store) == E_NOTIMPL && !store, 5);

	modal = TRUE;
	check(API(DialogBoxParamW)(instance, MAKEINTRESOURCEW(1), NULL, dialog_proc, -123) == -234, 20);
	check(API(DialogBoxParamA)(instance, MAKEINTRESOURCEA(1), NULL, dialog_proc, -123) == -234, 21);
	check(API(DialogBoxIndirectParamW)(instance, definition, NULL, dialog_proc, -123) == -234, 22);
	check(API(DialogBoxIndirectParamA)(instance, definition, NULL, dialog_proc, -123) == -234, 23);
	modal = FALSE;
	dialog = API(CreateDialogParamW)(instance, MAKEINTRESOURCEW(1), NULL, dialog_proc, -123);
	check(dialog != NULL, 30);
	item = API(GetDlgItem)(dialog, 100);
	check(item && API(GetDlgCtrlID)(item) == 100, 31);
	check(API(SetDlgItemTextW)(dialog, 100, L"wide"), 32);
	check(API(GetDlgItemTextW)(dialog, 100, wide, 32) == 4 && wide[0] == 'w' && !wide[4], 33);
	check(API(SetDlgItemTextA)(dialog, 100, "ansi"), 34);
	check(API(GetDlgItemTextA)(dialog, 100, ansi, 32) == 4 && ansi[0] == 'a' && !ansi[4], 35);
	check(API(SetDlgItemInt)(dialog, 100, (UINT)-42, TRUE), 36);
	check(API(GetDlgItemInt)(dialog, 100, &translated, TRUE) == (UINT)-42 && translated, 37);
	check(API(CheckDlgButton)(dialog, 101, BST_CHECKED), 38);
	check(API(IsDlgButtonChecked)(dialog, 101) == BST_CHECKED, 39);
	check(API(SendDlgItemMessageW)(dialog, 100, WM_GETTEXTLENGTH, 0, 0) == 3, 40);
	check(API(SendDlgItemMessageA)(dialog, 100, WM_GETTEXT, 32, (LPARAM)ansi) == 3 && ansi[0] == '-', 41);
	check(API(MapDialogRect)(dialog, &rect) && rect.right > 0 && rect.bottom > 0, 42);
	check(LOWORD(API(SendMessageW)(dialog, DM_GETDEFID, 0, 0)) == IDOK, 43);
	message.hwnd = item;
	message.message = WM_KEYDOWN;
	message.wParam = VK_TAB;
	check(API(IsDialogMessageW)(dialog, &message), 44);
	check(API(IsDialogMessageA)(dialog, &message), 45);
	check(API(DestroyWindow)(dialog), 46);
	dialog = API(CreateDialogParamA)(instance, MAKEINTRESOURCEA(1), NULL, dialog_proc, -123);
	check(dialog && API(DestroyWindow)(dialog), 47);
	dialog = API(CreateDialogIndirectParamW)(instance, definition, NULL, dialog_proc, -123);
	check(dialog && API(DestroyWindow)(dialog), 48);
	dialog = API(CreateDialogIndirectParamA)(instance, definition, NULL, dialog_proc, -123);
	check(dialog && API(DestroyWindow)(dialog), 49);
	check(initialized == 8, 50);
	return 0;
}
