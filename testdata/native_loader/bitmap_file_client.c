#define COBJMACROS
#include <windows.h>
#include <olectl.h>

static unsigned fonts;
static int CALLBACK font_proc(const LOGFONTW *font, const TEXTMETRICW *metrics, DWORD type, LPARAM parameter)
{
	if (!font || !metrics || parameter != 123) ExitProcess(20);
	++fonts;
	return 0;
}

UINT raven_entry(void)
{
	/* A 2x2 24-bit BMP with padded scanlines: red/blue below green/white. */
	static const BYTE image[70] = {
		'B','M',70,0,0,0,0,0,0,0,54,0,0,0,40,0,0,0,2,0,0,0,2,0,0,0,1,0,24,0,
		0,0,0,0,16,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
		0,0,255,255,0,0,0,0,0,255,0,255,255,255,0,0
	};
	WCHAR temporary[MAX_PATH], file[MAX_PATH];
	char ansi[MAX_PATH];
	DWORD written;
	BITMAP bitmap;
	BITMAPINFO info = {0};
	BYTE pixels[16];
	if (!GetTempPathW(MAX_PATH, temporary) || !GetTempFileNameW(temporary, L"bmp", 0, file)) return 1;
	HANDLE handle = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE || !WriteFile(handle, image, sizeof(image), &written, NULL) || written != sizeof(image)) return 2;
	CloseHandle(handle);
	HBITMAP loaded = LoadImageW(NULL, file, IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE);
	if (!loaded || GetObjectW(loaded, sizeof(bitmap), &bitmap) != sizeof(bitmap) || bitmap.bmWidth != 2 || bitmap.bmHeight != 2) return 3;
	HDC dc = CreateCompatibleDC(NULL);
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = 2;
	info.bmiHeader.biHeight = 2;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 24;
	if (!GetDIBits(dc, loaded, 0, 0, NULL, &info, DIB_RGB_COLORS) || info.bmiHeader.biSizeImage != sizeof(pixels)) return 4;
	if (GetDIBits(dc, loaded, 0, 2, pixels, &info, DIB_RGB_COLORS) != 2 || pixels[0] || pixels[1] || pixels[2] != 255 || pixels[9] != 255) return 5;
	DeleteObject(loaded);
	if (!WideCharToMultiByte(CP_ACP, 0, file, -1, ansi, MAX_PATH, NULL, NULL)) return 6;
	loaded = LoadImageA(NULL, ansi, IMAGE_BITMAP, 4, 6, LR_LOADFROMFILE);
	if (!loaded || GetObjectW(loaded, sizeof(bitmap), &bitmap) != sizeof(bitmap) || bitmap.bmWidth != 4 || bitmap.bmHeight != 6) return 7;
	DeleteObject(loaded);
	/* The desktop's callback and section sources must be connected at either width. */
	EnumFontsW(dc, NULL, font_proc, 123);
	if (!fonts) return 8;
	HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, NULL);
	BYTE *view = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, 4096), *bits = NULL;
	info.bmiHeader.biHeight = -2;
	info.bmiHeader.biBitCount = 32;
	loaded = CreateDIBSection(dc, &info, DIB_RGB_COLORS, (void **)&bits, section, 0);
	if (!section || !view || !loaded || !bits) return 9;
	bits[0] = 42;
	if (view[0] != 42) return 10;
	DeleteObject(loaded);
	UnmapViewOfFile(view);
	CloseHandle(section);
#ifdef RAVEN_TEST_OLE
	const GUID picture_id = {0x7bf80980,0xbf32,0x101a,{0x8b,0xbb,0,0xaa,0,0x30,0x0c,0xab}};
	HRESULT (WINAPI *load_picture)(LPOLESTR, LPUNKNOWN, DWORD, OLE_COLOR, REFIID, void **);
	IPicture *picture;
	OLE_HANDLE picture_handle;
	load_picture = (void *)GetProcAddress(LoadLibraryA("oleaut32.dll"), "OleLoadPicturePath");
	if (!load_picture || load_picture(file, NULL, 0, 0, &picture_id, (void **)&picture) != S_OK) return 11;
	if (IPicture_get_Handle(picture, &picture_handle) != S_OK || !picture_handle) return 12;
	if (GetObjectW((HBITMAP)(ULONG_PTR)picture_handle, sizeof(bitmap), &bitmap) != sizeof(bitmap) || !bitmap.bmBits) return 13;
	if (((BYTE *)bitmap.bmBits)[2] != 255) return 14;
	info.bmiHeader.biBitCount = 0;
	if (!GetDIBits(dc, (HBITMAP)(ULONG_PTR)picture_handle, 0, 0, NULL, &info, DIB_RGB_COLORS) || info.bmiHeader.biBitCount != 24 || info.bmiHeader.biSizeImage != 16) return 15;
	IPicture_Release(picture);
#endif
	DeleteDC(dc);
	DeleteFileW(file);
	return 0;
}
