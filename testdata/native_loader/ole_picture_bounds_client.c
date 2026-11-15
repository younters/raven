/* OleLoadPicture keeps a bitmap's own pixels in a DIB section. A header that promises more color table or rows than the data holds is
 * refused instead of being read past the end (Raven's choice of failure; the test checks only that loading fails). Returns 0, or the
 * number of the first failing check. */
#include <windows.h>
#include <ole2.h>
#include <olectl.h>

static HRESULT load(const void *bytes, SIZE_T size)
{
	HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
	IStream *stream = NULL;
	IPicture *picture = NULL;
	HRESULT status;
	BYTE *target = memory ? GlobalLock(memory) : NULL;
	if (!target)
		return E_OUTOFMEMORY;
	for (SIZE_T at = 0; at < size; ++at)
		target[at] = ((const BYTE *)bytes)[at];
	GlobalUnlock(memory);
	if (CreateStreamOnHGlobal(memory, TRUE, &stream))
		return E_UNEXPECTED;
	status = OleLoadPicture(stream, (LONG)size, FALSE, &IID_IPicture, (void **)&picture);
	if (picture)
		picture->lpVtbl->Release(picture);
	stream->lpVtbl->Release(stream);
	return status;
}

static void header(BYTE *at, LONG width, LONG height, WORD bits)
{
	BITMAPINFOHEADER *info = (BITMAPINFOHEADER *)at;
	info->biSize = sizeof *info;
	info->biWidth = width;
	info->biHeight = height;
	info->biPlanes = 1;
	info->biBitCount = bits;
	info->biCompression = BI_RGB;
	info->biClrUsed = 0;
}

static void file_header(BYTE *at, DWORD size, DWORD offset)
{
	BITMAPFILEHEADER *file = (BITMAPFILEHEADER *)at;
	file->bfType = 0x4d42;
	file->bfSize = size;
	file->bfOffBits = offset;
}

int raven_entry(void)
{
	enum { info_end = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) };
	static BYTE file[info_end + 256 * sizeof(RGBQUAD) + 16];
	static BYTE dib[sizeof(BITMAPINFOHEADER) + 16];
	if (FAILED(CoInitialize(NULL)))
		return 1;
	/* Whole 2x2 32-bit pictures load, bottom-up and top-down, as a file and as a headerless DIB. */
	file_header(file, info_end + 16, info_end);
	header(file + sizeof(BITMAPFILEHEADER), 2, 2, 32);
	if (FAILED(load(file, info_end + 16)))
		return 2;
	header(file + sizeof(BITMAPFILEHEADER), 2, -2, 32);
	if (FAILED(load(file, info_end + 16)))
		return 3;
	header(dib, 2, 2, 32);
	if (FAILED(load(dib, sizeof dib)))
		return 4;
	/* An 8-bit file with its whole 256-entry table and one row of four pixels loads. */
	file_header(file, info_end + 1024 + 4, info_end + 1024);
	header(file + sizeof(BITMAPFILEHEADER), 4, 1, 8);
	if (FAILED(load(file, info_end + 1024 + 4)))
		return 5;
	/* The same file without its table: the four pixel bytes fit, the table does not. */
	file_header(file, info_end + 4, info_end);
	if (SUCCEEDED(load(file, info_end + 4)))
		return 6;
	/* A file whose rows run past the end: 2x3 at 32 bits needs 24 bytes after bfOffBits, 16 are there. */
	file_header(file, info_end + 16, info_end);
	header(file + sizeof(BITMAPFILEHEADER), 2, 3, 32);
	if (SUCCEEDED(load(file, info_end + 16)))
		return 7;
	/* A headerless DIB that claims a million pixels with none present. */
	header(dib, 1000, 1000, 32);
	if (SUCCEEDED(load(dib, sizeof(BITMAPINFOHEADER))))
		return 8;
	/* A headerless DIB with a table larger than the data (biClrUsed counts its entries). */
	header(dib, 1, 1, 32);
	((BITMAPINFOHEADER *)dib)->biClrUsed = 0x40000000;
	if (SUCCEEDED(load(dib, sizeof dib)))
		return 9;
	CoUninitialize();
	return 0;
}
