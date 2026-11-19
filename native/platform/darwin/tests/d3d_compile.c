#define COBJMACROS
#include <windows.h>
#include <d3dcommon.h>

// ANGLE's path to the HLSL compiler: d3dcompiler_47 loaded at run time from System32, and shader model 5 bytecode out of D3DCompile.
typedef HRESULT (WINAPI *compile_fn)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *, UINT, UINT,
    ID3DBlob **, ID3DBlob **);

static const char vertex[] = "float4 main(float4 p : POSITION) : SV_Position { return p * 2.0; }";
static const char pixel[] = "Texture2D t; SamplerState s; float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target"
                            " { return t.Sample(s, uv) * float4(1, 0.5, 0.25, 1); }";

static int compile(compile_fn fn, const char *source, SIZE_T size, const char *profile)
{
    ID3DBlob *code = NULL, *errors = NULL;
    if (FAILED(fn(source, size, "shader", NULL, NULL, "main", profile, 0, 0, &code, &errors)) || !code) return 0;
    const DWORD *words = (const DWORD *)ID3D10Blob_GetBufferPointer(code);
    const int ok = ID3D10Blob_GetBufferSize(code) > 32 && words[0] == 0x43425844; // "DXBC"
    ID3D10Blob_Release(code);
    if (errors) ID3D10Blob_Release(errors);
    return ok;
}

void entry(void)
{
    HMODULE library = LoadLibraryExW(L"d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    compile_fn fn = library ? (compile_fn)GetProcAddress(library, "D3DCompile") : NULL;
    if (!fn) ExitProcess(10);
    if (!compile(fn, vertex, sizeof vertex - 1, "vs_5_0")) ExitProcess(11);
    if (!compile(fn, pixel, sizeof pixel - 1, "ps_5_0")) ExitProcess(12);
    ID3DBlob *code = NULL, *errors = NULL;
    if (SUCCEEDED(fn("float4 main( : SV_Target", 24, "bad", NULL, NULL, "main", "ps_5_0", 0, 0, &code, &errors)) || code || !errors)
        ExitProcess(13);
    ID3D10Blob_Release(errors);
    HANDLE file = CreateFileA("C:\\d3d_compile.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD written;
    const char text[] = "OK: System32 d3dcompiler_47 compiles vs_5_0 and ps_5_0 to DXBC and reports errors";
    if (file == INVALID_HANDLE_VALUE || !WriteFile(file, text, sizeof text - 1, &written, NULL)) ExitProcess(14);
    CloseHandle(file);
    ExitProcess(0);
}
