/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mirko Brombin
 * See LICENSES/MIT.txt.
 */
/* Raven shell component: the OLE clipboard over the Win32 one.
 *
 * OleSetClipboard renders every format the data object offers in a global block onto the Win32 clipboard at once, as
 * OleFlushClipboard would; the object stays the current clipboard object until another replaces it. OleGetClipboard hands out
 * a data object that reads the Win32 clipboard when asked, so what other programs (and the host) put there is what it offers. */
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <ole2.h>

static IDataObject *current;
/* The clipboard's sequence number once the object was rendered: once it moves on, someone else owns the clipboard. */
static DWORD rendered;

/* The program's object, while the clipboard still holds what it rendered. */
static IDataObject *still_current(void)
{
	if (current && GetClipboardSequenceNumber() != rendered) {
		IDataObject_Release(current);
		current = NULL;
	}
	return current;
}

/* A copy of a global block, for a clipboard that keeps what it is given. */
static HGLOBAL copy_global(HGLOBAL source)
{
	const SIZE_T size = GlobalSize(source);
	const void *from = GlobalLock(source);
	HGLOBAL copy = from ? GlobalAlloc(GMEM_MOVEABLE, size) : NULL;
	void *to = copy ? GlobalLock(copy) : NULL;
	if (to) {
		memcpy(to, from, size);
		GlobalUnlock(copy);
	} else if (copy) {
		GlobalFree(copy);
		copy = NULL;
	}
	if (from)
		GlobalUnlock(source);
	return copy;
}

/* One format of the object onto the open clipboard, when it comes as a global block. */
static void render(IDataObject *data, FORMATETC *format)
{
	STGMEDIUM medium;
	if (!(format->tymed & TYMED_HGLOBAL))
		return;
	format->tymed = TYMED_HGLOBAL;
	if (FAILED(IDataObject_GetData(data, format, &medium)))
		return;
	if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal) {
		HGLOBAL copy = copy_global(medium.hGlobal);
		if (copy && !SetClipboardData(format->cfFormat, copy))
			GlobalFree(copy);
	}
	ReleaseStgMedium(&medium);
}

HRESULT WINAPI OleSetClipboard(IDataObject *data)
{
	if (!OpenClipboard(NULL))
		return CLIPBRD_E_CANT_OPEN;
	EmptyClipboard();
	if (current)
		IDataObject_Release(current);
	current = data;
	if (data) {
		IEnumFORMATETC *formats = NULL;
		IDataObject_AddRef(data);
		if (SUCCEEDED(IDataObject_EnumFormatEtc(data, DATADIR_GET, &formats)) && formats) {
			FORMATETC format;
			while (IEnumFORMATETC_Next(formats, 1, &format, NULL) == S_OK) {
				render(data, &format);
				if (format.ptd)
					CoTaskMemFree(format.ptd);
			}
			IEnumFORMATETC_Release(formats);
		} else {
			/* An object that cannot list its formats is asked for text. */
			FORMATETC text = {CF_UNICODETEXT, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
			render(data, &text);
			text.cfFormat = CF_TEXT;
			text.tymed = TYMED_HGLOBAL;
			render(data, &text);
		}
	}
	CloseClipboard();
	rendered = GetClipboardSequenceNumber();
	return S_OK;
}

HRESULT WINAPI OleIsCurrentClipboard(IDataObject *data)
{
	return data && data == still_current() ? S_OK : S_FALSE;
}

/* Everything is on the clipboard already; the object is let go. */
HRESULT WINAPI OleFlushClipboard(void)
{
	if (current) {
		IDataObject_Release(current);
		current = NULL;
	}
	return S_OK;
}

void WINAPI ReleaseStgMedium(STGMEDIUM *medium)
{
	if (!medium)
		return;
	switch (medium->tymed) {
	case TYMED_HGLOBAL:
		if (medium->hGlobal && !medium->pUnkForRelease)
			GlobalFree(medium->hGlobal);
		break;
	case TYMED_FILE:
		if (medium->lpszFileName) {
			if (!medium->pUnkForRelease)
				DeleteFileW(medium->lpszFileName);
			CoTaskMemFree(medium->lpszFileName);
		}
		break;
	case TYMED_ISTREAM:
		if (medium->pstm)
			IStream_Release(medium->pstm);
		break;
	case TYMED_ISTORAGE:
		if (medium->pstg)
			IStorage_Release(medium->pstg);
		break;
	case TYMED_GDI:
		if (medium->hBitmap && !medium->pUnkForRelease)
			DeleteObject(medium->hBitmap);
		break;
	case TYMED_MFPICT:
		if (medium->hMetaFilePict && !medium->pUnkForRelease) {
			METAFILEPICT *picture = GlobalLock(medium->hMetaFilePict);
			if (picture) {
				DeleteMetaFile(picture->hMF);
				GlobalUnlock(medium->hMetaFilePict);
			}
			GlobalFree(medium->hMetaFilePict);
		}
		break;
	case TYMED_ENHMF:
		if (medium->hEnhMetaFile && !medium->pUnkForRelease)
			DeleteEnhMetaFile(medium->hEnhMetaFile);
		break;
	}
	if (medium->pUnkForRelease)
		IUnknown_Release(medium->pUnkForRelease);
	medium->tymed = TYMED_NULL;
	medium->pUnkForRelease = NULL;
}

/* The formats on the clipboard when the enumerator was made, one at a time. */
typedef struct {
	IEnumFORMATETC iface;
	LONG references;
	UINT *formats;
	ULONG count, position;
} Formats;

static HRESULT WINAPI formats_query(IEnumFORMATETC *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (InlineIsEqualGUID(iid, &IID_IUnknown) || InlineIsEqualGUID(iid, &IID_IEnumFORMATETC)) {
		*out = self;
		IEnumFORMATETC_AddRef(self);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}
static ULONG WINAPI formats_add_ref(IEnumFORMATETC *self) { return InterlockedIncrement(&((Formats *)self)->references); }
static ULONG WINAPI formats_release(IEnumFORMATETC *self)
{
	Formats *formats = (Formats *)self;
	const LONG left = InterlockedDecrement(&formats->references);
	if (!left) {
		HeapFree(GetProcessHeap(), 0, formats->formats);
		HeapFree(GetProcessHeap(), 0, formats);
	}
	return left;
}
static HRESULT WINAPI formats_next(IEnumFORMATETC *self, ULONG wanted, FORMATETC *out, ULONG *fetched)
{
	Formats *formats = (Formats *)self;
	ULONG given = 0;
	if (!out || (wanted > 1 && !fetched))
		return E_INVALIDARG;
	for (; given < wanted && formats->position < formats->count; ++given, ++formats->position) {
		const FORMATETC format = {(CLIPFORMAT)formats->formats[formats->position], NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
		out[given] = format;
	}
	if (fetched)
		*fetched = given;
	return given == wanted ? S_OK : S_FALSE;
}
static HRESULT WINAPI formats_skip(IEnumFORMATETC *self, ULONG count)
{
	Formats *formats = (Formats *)self;
	const ULONG left = formats->count - formats->position;
	formats->position += count < left ? count : left;
	return count <= left ? S_OK : S_FALSE;
}
static HRESULT WINAPI formats_reset(IEnumFORMATETC *self)
{
	((Formats *)self)->position = 0;
	return S_OK;
}
static HRESULT make_formats(const UINT *list, ULONG count, ULONG position, IEnumFORMATETC **out);
static HRESULT WINAPI formats_clone(IEnumFORMATETC *self, IEnumFORMATETC **out)
{
	Formats *formats = (Formats *)self;
	return out ? make_formats(formats->formats, formats->count, formats->position, out) : E_POINTER;
}
static IEnumFORMATETCVtbl formats_vtbl = {formats_query, formats_add_ref, formats_release, formats_next, formats_skip, formats_reset,
	formats_clone};

static HRESULT make_formats(const UINT *list, ULONG count, ULONG position, IEnumFORMATETC **out)
{
	Formats *formats = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *formats);
	if (formats)
		formats->formats = HeapAlloc(GetProcessHeap(), 0, (count ? count : 1) * sizeof *list);
	if (!formats || !formats->formats) {
		if (formats)
			HeapFree(GetProcessHeap(), 0, formats);
		*out = NULL;
		return E_OUTOFMEMORY;
	}
	formats->iface.lpVtbl = &formats_vtbl;
	formats->references = 1;
	memcpy(formats->formats, list, count * sizeof *list);
	formats->count = count;
	formats->position = position;
	*out = &formats->iface;
	return S_OK;
}

/* The clipboard as a data object: each request reads what the clipboard holds then. */
typedef struct {
	IDataObject iface;
	LONG references;
} Clipboard;

static HRESULT WINAPI clipboard_query(IDataObject *self, REFIID iid, void **out)
{
	if (!out)
		return E_POINTER;
	if (InlineIsEqualGUID(iid, &IID_IUnknown) || InlineIsEqualGUID(iid, &IID_IDataObject)) {
		*out = self;
		IDataObject_AddRef(self);
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}
static ULONG WINAPI clipboard_add_ref(IDataObject *self) { return InterlockedIncrement(&((Clipboard *)self)->references); }
static ULONG WINAPI clipboard_release(IDataObject *self)
{
	const LONG left = InterlockedDecrement(&((Clipboard *)self)->references);
	if (!left)
		HeapFree(GetProcessHeap(), 0, self);
	return left;
}
static HRESULT WINAPI clipboard_get_data(IDataObject *self, FORMATETC *format, STGMEDIUM *medium)
{
	HGLOBAL copy = NULL;
	if (!format || !medium)
		return E_INVALIDARG;
	if (!(format->tymed & TYMED_HGLOBAL))
		return DV_E_TYMED;
	if (!IsClipboardFormatAvailable(format->cfFormat))
		return DV_E_FORMATETC;
	if (!OpenClipboard(NULL))
		return CLIPBRD_E_CANT_OPEN;
	{
		HANDLE held = GetClipboardData(format->cfFormat);
		if (held)
			copy = copy_global(held);
	}
	CloseClipboard();
	if (!copy)
		return DV_E_FORMATETC;
	medium->tymed = TYMED_HGLOBAL;
	medium->hGlobal = copy;
	medium->pUnkForRelease = NULL;
	return S_OK;
}
static HRESULT WINAPI clipboard_get_data_here(IDataObject *self, FORMATETC *format, STGMEDIUM *medium) { return E_NOTIMPL; }
static HRESULT WINAPI clipboard_query_get_data(IDataObject *self, FORMATETC *format)
{
	if (!format)
		return E_INVALIDARG;
	if (!(format->tymed & TYMED_HGLOBAL))
		return DV_E_TYMED;
	return IsClipboardFormatAvailable(format->cfFormat) ? S_OK : DV_E_FORMATETC;
}
static HRESULT WINAPI clipboard_canonical(IDataObject *self, FORMATETC *in, FORMATETC *out)
{
	if (out)
		out->ptd = NULL;
	return DATA_S_SAMEFORMATETC;
}
static HRESULT WINAPI clipboard_set_data(IDataObject *self, FORMATETC *format, STGMEDIUM *medium, BOOL release) { return E_NOTIMPL; }
static HRESULT WINAPI clipboard_enum(IDataObject *self, DWORD direction, IEnumFORMATETC **out)
{
	UINT list[256];
	ULONG count = 0;
	if (!out)
		return E_POINTER;
	if (direction != DATADIR_GET) {
		*out = NULL;
		return E_NOTIMPL;
	}
	if (OpenClipboard(NULL)) {
		for (UINT format = EnumClipboardFormats(0); format && count < ARRAYSIZE(list); format = EnumClipboardFormats(format))
			list[count++] = format;
		CloseClipboard();
	}
	return make_formats(list, count, 0, out);
}
static HRESULT WINAPI clipboard_advise(IDataObject *self, FORMATETC *format, DWORD flags, IAdviseSink *sink, DWORD *connection)
{
	return OLE_E_ADVISENOTSUPPORTED;
}
static HRESULT WINAPI clipboard_unadvise(IDataObject *self, DWORD connection) { return OLE_E_ADVISENOTSUPPORTED; }
static HRESULT WINAPI clipboard_enum_advise(IDataObject *self, IEnumSTATDATA **out) { return OLE_E_ADVISENOTSUPPORTED; }
static IDataObjectVtbl clipboard_vtbl = {clipboard_query, clipboard_add_ref, clipboard_release, clipboard_get_data,
	clipboard_get_data_here, clipboard_query_get_data, clipboard_canonical, clipboard_set_data, clipboard_enum, clipboard_advise,
	clipboard_unadvise, clipboard_enum_advise};

HRESULT WINAPI OleGetClipboard(IDataObject **out)
{
	Clipboard *clipboard;
	if (!out)
		return E_INVALIDARG;
	/* The program's own object, while it is still the clipboard's, answers with everything it can render. */
	if (still_current()) {
		*out = current;
		IDataObject_AddRef(current);
		return S_OK;
	}
	clipboard = HeapAlloc(GetProcessHeap(), 0, sizeof *clipboard);
	if (!clipboard) {
		*out = NULL;
		return E_OUTOFMEMORY;
	}
	clipboard->iface.lpVtbl = &clipboard_vtbl;
	clipboard->references = 1;
	*out = &clipboard->iface;
	return S_OK;
}
