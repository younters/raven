/* Copies text through the OLE clipboard as Firefox does and reads it back through OLE and Win32, then checks that a Win32 copy
 * by someone else replaces the program's object. Returns 0, or the number of the first failing check. */
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <ole2.h>

static const WCHAR copied[] = {'r', 'a', 'v', 'e', 'n', ' ', 'o', 'l', 'e', 0};
static const WCHAR replaced[] = {'w', 'i', 'n', '3', '2', 0};
static LONG references = 1, enumerated;

static int same_text(const WCHAR *left, const WCHAR *right)
{
	for (;; ++left, ++right) {
		if (*left != *right)
			return 0;
		if (!*left)
			return 1;
	}
}

static HGLOBAL text_block(const WCHAR *text, SIZE_T bytes)
{
	HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, bytes);
	WCHAR *to = block ? GlobalLock(block) : NULL;
	if (!to)
		return NULL;
	for (SIZE_T index = 0; index < bytes / 2; ++index)
		to[index] = text[index];
	GlobalUnlock(block);
	return block;
}

/* One format, Unicode text in global memory, enumerated once. */
static HRESULT WINAPI formats_query(IEnumFORMATETC *self, REFIID iid, void **out) { *out = self; return S_OK; }
static ULONG WINAPI formats_reference(IEnumFORMATETC *self) { return 1; }
static HRESULT WINAPI formats_next(IEnumFORMATETC *self, ULONG wanted, FORMATETC *out, ULONG *fetched)
{
	if (enumerated++) {
		if (fetched)
			*fetched = 0;
		return S_FALSE;
	}
	out->cfFormat = CF_UNICODETEXT;
	out->ptd = NULL;
	out->dwAspect = DVASPECT_CONTENT;
	out->lindex = -1;
	out->tymed = TYMED_HGLOBAL;
	if (fetched)
		*fetched = 1;
	return wanted == 1 ? S_OK : S_FALSE;
}
static HRESULT WINAPI formats_skip(IEnumFORMATETC *self, ULONG count) { return S_FALSE; }
static HRESULT WINAPI formats_reset(IEnumFORMATETC *self) { enumerated = 0; return S_OK; }
static HRESULT WINAPI formats_clone(IEnumFORMATETC *self, IEnumFORMATETC **out) { return E_NOTIMPL; }
static IEnumFORMATETCVtbl formats_vtbl = {formats_query, formats_reference, formats_reference, formats_next, formats_skip,
	formats_reset, formats_clone};
static IEnumFORMATETC formats = {&formats_vtbl};

static HRESULT WINAPI data_query(IDataObject *self, REFIID iid, void **out) { *out = self; InterlockedIncrement(&references); return S_OK; }
static ULONG WINAPI data_add_ref(IDataObject *self) { return InterlockedIncrement(&references); }
static ULONG WINAPI data_release(IDataObject *self) { return InterlockedDecrement(&references); }
static HRESULT WINAPI data_get(IDataObject *self, FORMATETC *format, STGMEDIUM *medium)
{
	if (format->cfFormat != CF_UNICODETEXT || !(format->tymed & TYMED_HGLOBAL))
		return DV_E_FORMATETC;
	medium->tymed = TYMED_HGLOBAL;
	medium->hGlobal = text_block(copied, sizeof copied);
	medium->pUnkForRelease = NULL;
	return medium->hGlobal ? S_OK : E_OUTOFMEMORY;
}
static HRESULT WINAPI data_get_here(IDataObject *self, FORMATETC *format, STGMEDIUM *medium) { return E_NOTIMPL; }
static HRESULT WINAPI data_query_get(IDataObject *self, FORMATETC *format)
{
	return format->cfFormat == CF_UNICODETEXT ? S_OK : DV_E_FORMATETC;
}
static HRESULT WINAPI data_canonical(IDataObject *self, FORMATETC *in, FORMATETC *out) { return DATA_S_SAMEFORMATETC; }
static HRESULT WINAPI data_set(IDataObject *self, FORMATETC *format, STGMEDIUM *medium, BOOL release) { return E_NOTIMPL; }
static HRESULT WINAPI data_enum(IDataObject *self, DWORD direction, IEnumFORMATETC **out)
{
	enumerated = 0;
	*out = &formats;
	return S_OK;
}
static HRESULT WINAPI data_advise(IDataObject *self, FORMATETC *format, DWORD flags, IAdviseSink *sink, DWORD *connection)
{
	return OLE_E_ADVISENOTSUPPORTED;
}
static HRESULT WINAPI data_unadvise(IDataObject *self, DWORD connection) { return OLE_E_ADVISENOTSUPPORTED; }
static HRESULT WINAPI data_enum_advise(IDataObject *self, IEnumSTATDATA **out) { return OLE_E_ADVISENOTSUPPORTED; }
static IDataObjectVtbl data_vtbl = {data_query, data_add_ref, data_release, data_get, data_get_here, data_query_get, data_canonical,
	data_set, data_enum, data_advise, data_unadvise, data_enum_advise};
static IDataObject source = {&data_vtbl};

/* The Unicode text a data object gives, compared with what is expected. */
static int object_text(IDataObject *data, const WCHAR *expected)
{
	FORMATETC format = {CF_UNICODETEXT, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
	STGMEDIUM medium;
	if (FAILED(IDataObject_GetData(data, &format, &medium)) || medium.tymed != TYMED_HGLOBAL)
		return 0;
	const WCHAR *text = GlobalLock(medium.hGlobal);
	const int same = text && same_text(text, expected);
	GlobalUnlock(medium.hGlobal);
	ReleaseStgMedium(&medium);
	return same;
}

static int clipboard_text(const WCHAR *expected)
{
	if (!OpenClipboard(NULL))
		return 0;
	HANDLE held = GetClipboardData(CF_UNICODETEXT);
	const WCHAR *text = held ? GlobalLock(held) : NULL;
	const int same = text && same_text(text, expected);
	if (text)
		GlobalUnlock(held);
	CloseClipboard();
	return same;
}

unsigned int raven_entry(void)
{
	if (FAILED(OleInitialize(NULL)))
		return 1;
	if (OleSetClipboard(&source) != S_OK)
		return 2;
	if (OleIsCurrentClipboard(&source) != S_OK)
		return 3;
	if (!clipboard_text(copied))
		return 4;
	IDataObject *reading = NULL;
	if (OleGetClipboard(&reading) != S_OK || !reading || !object_text(reading, copied))
		return 5;
	IDataObject_Release(reading);
	/* Someone else's Win32 copy takes the clipboard over. */
	if (!OpenClipboard(NULL) || !EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, text_block(replaced, sizeof replaced)) ||
		!CloseClipboard())
		return 6;
	if (OleIsCurrentClipboard(&source) != S_FALSE)
		return 7;
	reading = NULL;
	if (OleGetClipboard(&reading) != S_OK || !reading || reading == &source || !object_text(reading, replaced))
		return 8;
	FORMATETC missing = {CF_BITMAP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
	if (IDataObject_QueryGetData(reading, &missing) == S_OK)
		return 9;
	IEnumFORMATETC *listed = NULL;
	FORMATETC first;
	if (FAILED(IDataObject_EnumFormatEtc(reading, DATADIR_GET, &listed)) || !listed ||
		IEnumFORMATETC_Next(listed, 1, &first, NULL) != S_OK)
		return 10;
	IEnumFORMATETC_Release(listed);
	IDataObject_Release(reading);
	/* Setting the clipboard to nothing empties it and lets the old object go. */
	if (OleSetClipboard(NULL) != S_OK || OleFlushClipboard() != S_OK || IsClipboardFormatAvailable(CF_UNICODETEXT))
		return 11;
	if (references != 1)
		return 12;
	OleUninitialize();
	return 0;
}
