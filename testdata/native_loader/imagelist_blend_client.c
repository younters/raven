#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>

int raven_entry(void)
{
	for (int alpha = 0; alpha < 2; ++alpha) {
		BITMAPINFO info = {0};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = 4; info.bmiHeader.biHeight = -2;
		info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
		DWORD *pixels;
		HBITMAP image = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
		BYTE mask_bits[] = {0x10, 0, 0x10, 0};
		HBITMAP mask = CreateBitmap(4, 2, 1, 1, mask_bits);
		if (!image || !mask) return 1;
		for (int i = 0; i < 8; ++i) pixels[i] = i % 4 == 3 ? 0 : 0x00c86428 | (alpha ? 0xff000000 : 0);
		HIMAGELIST images = ImageList_Create(4, 2, ILC_COLOR32 | ILC_MASK, 1, 1);
		if (!images || ImageList_Add(images, image, mask) != 0) return 2;
		HDC dc = CreateCompatibleDC(NULL);
		HBITMAP target = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, (void **)&pixels, NULL, 0);
		HGDIOBJ old = SelectObject(dc, target);
		for (int blend = 0; blend < 2; ++blend) {
			for (int i = 0; i < 8; ++i) pixels[i] = 0x00112233;
			if (!ImageList_DrawEx(images, 0, dc, 0, 0, 4, 2, CLR_NONE, RGB(20, 60, 220),
				ILD_TRANSPARENT | (blend ? ILD_BLEND50 : ILD_BLEND25))) return 3;
			/* Every opaque pixel gets the same tint; mask pixels preserve the destination. */
			const DWORD expected = blend ? 0x006e5082 : 0x009b5a55;
			for (int i = 0; i < 8; ++i)
				if ((pixels[i] & 0xffffff) != (i % 4 == 3 ? 0x00112233 : expected)) return 4 + alpha * 2 + blend;
		}
		SelectObject(dc, old); DeleteDC(dc); DeleteObject(target);
		ImageList_Destroy(images); DeleteObject(mask); DeleteObject(image);
	}
	return 0;
}
