# Graphics on macOS

| Windows API | Backend |
|---|---|
| Vulkan | Raven's x64 and x86 Vulkan bridge on the host driver |
| Direct3D 10 and 11 | [DXMT](DXMT.md) for x64 programs |
| DirectComposition | `dcomp-metal.dll` from the x64 DXMT source provisioner |
| Direct3D 9 | mtld3d for x64 and WoW64 on FEX, with a separately built overlay |
| Direct3D 12 | Mullion for x64, with explicit backend selection |

## Vulkan

The arm64 host loads Vulkan through the SDK loader. The Win32 surface calls use `VK_EXT_metal_surface` for local
Cocoa windows. Foreign windows can use `VK_EXT_headless_surface` when the driver offers it. Raven reads their
swapchain images back to shared pixels. The bridge enables portability enumeration and uses coherent shadow buffers
where Linux-style direct sharing is unavailable.

`RAVEN_VULKAN_TEST_ICD` selects the ICD for tests. The default path is
`/usr/local/share/vulkan/icd.d/MoltenVK_icd.json`. To try [KosmicKrisp](../../upstream/kosmickrisp/README.md), import its
pinned files from the Vulkan SDK and set `RAVEN_KOSMICKRISP_ROOT`. This selects the driver for tests. For a normal
launch, select the ICD using the Vulkan loader's configuration.

```sh
python3 native/platform/darwin/scripts/provision-kosmickrisp.py --sdk /path/to/VulkanSDK/macOS \
  --destination "$PWD/../raven-components/kosmickrisp"
cmake -S . -B build-macos -DRAVEN_KOSMICKRISP_ROOT="$PWD/../raven-components/kosmickrisp"
```

`native_vulkan_cocoa_present` checks pixels before and after resizing a Cocoa window. Enable it with
`RAVEN_DARWIN_PRESENT_TEST=ON` and run it in a logged-in desktop session with screen capture permission and Pillow.
Use `RAVEN_DARWIN_PRESENT_PYTHON` to select a Python installation with Pillow. The strict DXVK comparison tests
(`darwin_dxvk_*`) are opt-in through `RAVEN_DARWIN_DXVK_TESTS` and require `RAVEN_DXMT_ROOT` and `RAVEN_DXVK_PE64_DIR`.

## Direct3D backend selection

Raven can load the program's local graphics DLLs. For a program using DXMT, the configured root supplies the PE DLLs
and the matching unix library. `RAVEN_D3D_DIRECTORY` supplies additional Direct3D library directories.

For [Mullion](../../upstream/mullion/README.md), build the component with `mullion/build.py`. Select it per process
with `RAVEN_D3D12_BACKEND=mullion` and set `RAVEN_MULLION_ROOT` to the component directory. Use its DLLs as a set,
including its own DXGI.

## FEX graphics

FEX translates the Windows DLLs and calls the matching arm64 unix libraries in the arm64 Raven process.

- DXMT needs an arm64 overlay built from the matching DXMT source configuration with `dxmt/build_arm64.py`.
  Set `RAVEN_DXMT_ROOT` to the root containing the PE DLLs and `RAVEN_DXMT_ARM64_ROOT` to the overlay.
- For Direct3D 9, build the unix side with `mtld3d/build_arm64.py --source /path/to/mtld3d --destination /path/to/overlay`.
  Set `RAVEN_MTLD3D_ARM64_ROOT` to the overlay and provide matching `d3d9.dll`, `mtld3d.dll` and
  `vcruntime140.dll` beside the program. The [mtld3d record](../../upstream/mtld3d/README.md) specifies the source pin.
