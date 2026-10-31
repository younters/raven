// Diagnostic for the v0.15.1 unixlib ABI. This does not run a Windows D3D9 workload.
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

typedef int32_t (*unix_call)(void *);
struct device_info {
    uint64_t name_ptr, name_buf_len, name_len, registry_id;
    uint32_t caps, pad;
};
struct attach_layer {
    uint64_t hwnd, device;
    uint32_t width, height;
    uint64_t view, layer;
    uint32_t backing_scale, display_sync, hdr, color_space, max_fps, metalfx;
    uint64_t backing_scale_ptr;
    uint32_t software_cursor, software_cursor_active;
    uint64_t cursor_kick_ptr, client_surface_request_ptr;
};
_Static_assert(sizeof(struct device_info) == 40, "v0.15.1 GetDeviceInfo ABI");
_Static_assert(sizeof(struct attach_layer) == 96, "v0.15.1 AttachMetalLayer ABI");

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s /absolute/path/to/mtld3d.so\n", argv[0]);
        return 2;
    }
    @autoreleasepool {
        void *module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (!module) {
            fprintf(stderr, "dlopen: %s\n", dlerror());
            return 1;
        }
        unix_call *native = dlsym(module, "__wine_unix_call_funcs");
        unix_call *wow64 = dlsym(module, "__wine_unix_call_wow64_funcs");
        if (!native || !wow64) return 1;
        for (unsigned i = 0; i < 22; ++i)
            if (!native[i] || native[i] != wow64[i]) return 1;
        puts("native/wow64: 22 non-null entries, identical handlers");

        char name[256] = {0};
        struct device_info info = {.name_ptr = (uintptr_t)name, .name_buf_len = sizeof name};
        int32_t status = native[1](&info);
        printf("GetDeviceInfo: status=%08x device=%s registry=%llu caps=%08x\n",
            (uint32_t)status, name, (unsigned long long)info.registry_id, info.caps);
        if (status || !info.name_len || !info.registry_id) return 77;

        // No window-data entry can be called in this probe: the table must be absent.
        // A nonzero HWND and real device ensure attach reaches MacdrvFuncs::load.
        if (dlsym(RTLD_DEFAULT, "macdrv_functions")) {
            fputs("unexpected macdrv_functions; run in a clean standalone process\n", stderr);
            return 1;
        }
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) return 77;
        struct attach_layer attach = {.hwnd = 0x10000, .device = (uintptr_t)device,
            .width = 64, .height = 64};
        status = native[3](&attach);
        printf("AttachMetalLayer without macdrv_functions: status=%08x view=%llx layer=%llx\n",
            (uint32_t)status, (unsigned long long)attach.view, (unsigned long long)attach.layer);
        [device release];
        // Success here means the diagnostic reproduced the integration blocker.
        // It does not mean presentation passed. No renderer is selected in Raven.
        return (uint32_t)status == 0xc0000001U && !attach.view && !attach.layer ? 0 : 1;
    }
}
