#include <windows.h>
#include <ole2.h>
#include <olectl.h>

// OleLoadPicture keeps a bitmap's own pixels in a DIB section; headers that promise more palette or rows than the stream holds
// are rejected instead of being read past the data. Returns 0, or the number of the check that failed.
static HRESULT load(const void *bytes, SIZE_T size)
{
	HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
	IStream *stream = NULL;
	IPicture *picture = NULL;
	HRESULT status;
	BYTE *target = GlobalLock(memory);
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
}

int raven_entry(void)
{
	static BYTE file[sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + 16];
	static BYTE dib[sizeof(BITMAPINFOHEADER) + 16];
	BITMAPFILEHEADER *bmp = (BITMAPFILEHEADER *)file;
	if (CoInitialize(NULL) < 0)
		return 1;
	// A whole 2x2 32-bit file and headerless DIB load.
	bmp->bfType = 0x4d42;
	bmp->bfSize = sizeof file;
	bmp->bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
	header(file + sizeof(BITMAPFILEHEADER), 2, 2, 32);
	if (load(file, sizeof file))
		return 2;
	header(dib, 2, 2, 32);
	if (load(dib, sizeof dib))
		return 3;
	// An 8-bit file whose 256-entry palette is missing: the four pixel bytes fit, the palette does not.
	header(file + sizeof(BITMAPFILEHEADER), 4, 1, 8);
	if (SUCCEEDED(load(file, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + 4)))
		return 4;
	// A headerless DIB that claims a million pixels with none present.
	header(dib, 1000, 1000, 32);
	if (SUCCEEDED(load(dib, sizeof(BITMAPINFOHEADER))))
		return 5;
	CoUninitialize();
	return 0;
}
