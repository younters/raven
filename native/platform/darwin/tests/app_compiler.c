#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifdef COMPILER_DLL
__declspec(dllexport) HRESULT WINAPI D3DCompile(const void *source, SIZE_T size, const char *name, const void *defines,
    void *include, const char *entry, const char *target, UINT flags, UINT effect_flags, void **code, void **errors)
{
    (void)source; (void)size; (void)name; (void)defines; (void)include; (void)entry; (void)target;
    (void)flags; (void)effect_flags; (void)code; (void)errors;
    return 0x12345;
}
#else
int raven_entry(void)
{
    typedef HRESULT (WINAPI *Compile)(const void *,SIZE_T,const char *,const void *,void *,const char *,const char *,UINT,UINT,void **,void **);
    HMODULE compiler=LoadLibraryW(L"d3dcompiler_47.dll");
    Compile compile=compiler?(Compile)GetProcAddress(compiler,"D3DCompile"):NULL;
    if (!compile) return 1;
    return compile(NULL,0,NULL,NULL,NULL,NULL,NULL,0,0,NULL,NULL)==0x12345?0:2;
}
#endif
