# Vulkan on macOS

Verified on 2026-10-25 with an Apple M4 Pro, the LunarG loader 1.4.309, and MoltenVK 1.2.11 (device Vulkan version 1.2.296).
The arm64 host calls Vulkan; Windows x64 and x86 clients run in the Rosetta guest and use Raven's existing marshaling bridge.

## Integration

- A default macOS presentation selects Metal, including when there is no local window. Windows' surface extension maps to
  `VK_EXT_metal_surface`; foreign windows also enable `VK_EXT_headless_surface` when offered.
- Instance creation opts into portability enumeration. Device creation enables the driver's portability subset extension.
  Host reference calls in the tests make the same requests.
- Every target linked to `Vulkan::Vulkan` gets the SDK loader's rpath, including the standalone thunk-table test.
- `RAVEN_VULKAN_TEST_ICD` selects the test ICD; macOS defaults to `/usr/local/share/vulkan/icd.d/MoltenVK_icd.json`.
- Foreign presentation reads back real swapchain images and publishes shared pixels. The Darwin `SharedPixels` implementation
  maps a host-page-aligned range and exposes the requested 4 KiB section offset inside it. The file layout remains unchanged.
- Direct GPU allocation sharing through Linux `/proc` and DRM is unavailable. The bridge uses its existing coherent shadow
  fallback; passing `native_shared_gpu_memory_contract` verifies fallback selection, not zero-copy GPU memory sharing.
- Darwin spawn must explicitly inherit a control/socket descriptor already at its destination number: `dup2(fd, fd)` alone
  leaves close-on-exec set. GPU initialization can change which low descriptor is available.

## Tests

Run these serially because runtime tests share prefixes:

```sh
ctest --test-dir build -j1 --timeout 120 \
  -R 'native_(vulkan_|x86_vulkan_compute|shared_gpu_memory_contract)' --output-on-failure
```

Passing workflows:

- `native_vulkan_contract` and `native_vulkan_cli`: buffer transfers, compute, image transfers, triangle rendering,
  textured rendering, queries, extensions, and rejected invalid calls.
- `native_vulkan_extensions_x64` and `native_vulkan_extensions_x32`: extension commands and GPU compute results.
  The imageless framebuffer fixture supplies the required empty attachment-info chain even with zero attachments.
- `native_vulkan_foreign_present_x64` and `native_vulkan_foreign_present_x32`: actual GPU pixels, resize, and surface loss.
  The fixture allows up to 256 surface formats instead of assuming at most 32.
- `native_x86_vulkan_compute`: the normal loader and 32-bit thunk path, using the selected ICD instead of forcing lavapipe.
- `native_shared_gpu_memory_contract`: portability device setup and fallback selection on MoltenVK.
- `native_vulkan_thunk32_tables`: loader linkage and generated ABI tables.

Unsupported capabilities stay visible in CTest:

- `native_vulkan_queued_extensions_x64` is disabled on Darwin: the guest agent has no parallel guest threads or queued dispatch.
  The ordinary x64 extension workflow still runs and verifies its GPU results.
- `native_vulkan_descriptor_buffer_x64` and `native_vulkan_descriptor_buffer_x32` are skipped only when the fixture returns 3,
  meaning `VK_EXT_descriptor_buffer` was not offered. Other failures still fail. MoltenVK 1.2.11 does not offer this extension.

Additional CPU/x86, outer-surface, and desktop-surface smoke tests pass. The separate `native_shared_mapping_contract` test
still assumes `/proc/self/fd` for descriptor counting and read-only reopening, and fails on macOS before exercising mappings.

## Cocoa presentation check

The optional test needs an unlocked graphical login, screen capture access, and Python with Pillow:

```sh
cmake -S . -B build -DRAVEN_DARWIN_PRESENT_TEST=ON \
  -DRAVEN_DARWIN_PRESENT_PYTHON=/opt/homebrew/bin/python3
cmake --build build
ctest --test-dir build -j1 --timeout 120 -R native_vulkan_cocoa_present --output-on-failure
```

The independent Windows fixture creates a titled HWND, a Win32 Vulkan surface, a device, and a real FIFO swapchain. It renders
three frames with four colored render-pass clears at 256 by 192, then resizes the client to 320 by 200, replaces the swapchain,
and renders three changed frames. The test captures the actual desktop and compares each quadrant's interior in both phases.
It converts the display's ICC profile to sRGB and excludes window-corner clipping and edge antialiasing. Only matched client
crops are retained in `build/native/vulkan-cocoa-present/{first,resized}.png`; full desktop captures are deleted.

Both phases pass, including with `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`. The layer is the child view over the
client area returned by `CocoaPresentation::host_surface`; this test exercises that path instead of foreign-frame readback.

## Pinned Direct3D backends

The versions in `native/graphics/backends.json` were tested without changing their DLLs:

- **DXVK 3.1.1, D3D11:** `scripts/native-d3d11.py` fails device creation (`80004005`). Its DXGI log finds Apple M4 Pro and rejects
  it because it does not support Vulkan 1.3.
- **DXVK 3.1.1, D3D9:** the x64 `testdata/d3d9/window.c` workload reaches the same adapter rejection. DXVK throws and Raven
  subsequently reports the guest C++ exception (`20474343`); no D3D9 device is created.
- **vkd3d-proton 3.0.1, D3D12:** `scripts/native-d3d12.py` reports `hr=80004005`. The shipped dependency path uses DXVK's DXGI,
  which rejects the adapter at the same Vulkan 1.3 check before vkd3d's device setup. No D3D12 device is created.

The loader's Vulkan 1.4 version does not change the device's Vulkan 1.2 capabilities. Native `vulkaninfo` also reports false
for `geometryShader`, `shaderCullDistance`, `vulkanMemoryModel`, `robustBufferAccess2`, and `nullDescriptor`.
DXVK requires those features. Required extensions absent from this driver include `VK_EXT_depth_clip_enable`,
`VK_KHR_load_store_op_none`, `VK_KHR_maintenance4`, `VK_KHR_maintenance5`, `VK_KHR_maintenance6`, and
`VK_KHR_zero_initialize_workgroup_memory`.
See the [pinned DXVK capability checks](https://github.com/doitsujin/dxvk/blob/v3.1.1/src/dxvk/dxvk_device_info.cpp).

vkd3d-proton itself requires Vulkan 1.3 and robust buffer access / null descriptors from `VK_EXT_robustness2`, so removing DXGI's
version rejection would not make the installed MoltenVK sufficient. Its missing descriptor-buffer and mutable-descriptor-type
extensions are recommended capabilities, not mandatory requirements; they are not the basis for this conclusion.
See [vkd3d's pinned requirements](https://github.com/HansKristian-Work/vkd3d-proton/blob/v3.0.1/README.md#drivers) and
[feature validation](https://github.com/HansKristian-Work/vkd3d-proton/blob/v3.0.1/libs/vkd3d/device.c).

These are driver/backend capability limits. No Vulkan feature bits or API versions are fabricated by the bridge.
