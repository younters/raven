#define INITGUID
#define CINTERFACE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d2d1.h>
#include <uianimation.h>

int raven_entry(void)
{
	HMODULE d2d = LoadLibraryW(L"d2d1.dll"), animation = LoadLibraryW(L"uianimation.dll");
	HRESULT (WINAPI *factory)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS *, void **) = (void *)GetProcAddress(d2d, "D2D1CreateFactory");
	HRESULT (WINAPI *get_class)(REFCLSID, REFIID, void **) = (void *)GetProcAddress(animation, "DllGetClassObject");
	ID2D1Factory *draw = NULL;
	ID2D1RectangleGeometry *geometry = NULL;
	D2D1_RECT_F rectangle = {1, 2, 30, 40}, result = {0};
	if (!factory || !get_class || factory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &IID_ID2D1Factory, NULL, (void **)&draw) || !draw) return 1;
	if (draw->lpVtbl->CreateRectangleGeometry(draw, &rectangle, &geometry) || !geometry) return 2;
	geometry->lpVtbl->GetRect(geometry, &result);
	if (result.left != 1 || result.top != 2 || result.right != 30 || result.bottom != 40) return 3;
	IUnknown_Release((IUnknown *)geometry);
	IUnknown_Release((IUnknown *)draw);
	IClassFactory *class_factory = NULL;
	IUIAnimationManager *manager = NULL;
	IUIAnimationVariable *variable = NULL;
	DOUBLE value = 0;
	if (get_class(&CLSID_UIAnimationManager, &IID_IClassFactory, (void **)&class_factory) || !class_factory) return 4;
	if (IClassFactory_CreateInstance(class_factory, NULL, &IID_IUIAnimationManager, (void **)&manager) || !manager) return 5;
	if (IUIAnimationManager_CreateAnimationVariable(manager, 12.5, &variable) || !variable || IUIAnimationVariable_GetValue(variable, &value) || value != 12.5) return 6;
	IUIAnimationVariable_Release(variable);
	IUIAnimationManager_Release(manager);
	IClassFactory_Release(class_factory);
	HMODULE host = LoadLibraryW(L"ravenhost.dll");
	DWORD (WINAPI *create)(DWORD, DWORD) = (void *)GetProcAddress(host, "RavenCanvasCreate");
	DWORD (WINAPI *pixels)(DWORD, INT32, INT32, DWORD, DWORD, void *, DWORD, DWORD) = (void *)GetProcAddress(host, "RavenCanvasPixels");
	void (WINAPI *destroy)(DWORD) = (void *)GetProcAddress(host, "RavenCanvasDestroy");
	DWORD input = 0xff123456, output = 0;
	if (!create || !pixels || !destroy) return 7;
	DWORD canvas = create(1, 1);
	if (!canvas || !pixels(canvas, 0, 0, 1, 1, &input, 4, 1) || !pixels(canvas, 0, 0, 1, 1, &output, 4, 0) || input != output) return 8;
	destroy(canvas);
	return 0;
}
