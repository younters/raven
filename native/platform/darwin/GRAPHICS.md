# macOS graphics decision record

Research and local verification: **2026-10-25**. Host: Apple M4 Pro, macOS 27.2 (26B5091g), Xcode 27.1.
This is a private-runtime integration decision, not a claim that every Windows game or every API feature works.
Read [VULKAN.md](VULKAN.md) and [DXMT.md](DXMT.md) for the established transports.

## Decision

Keep a mixed backend strategy. The common Vulkan stack is a convergence target because both guest widths can use one native
arm64 driver through the existing bridge, without a native WoW64 unix-call table. DXMT's guest unixlib remains architecture-specific.
A direct Metal translator is useful, but the number of translation layers alone does not prove better performance. The experiment below establishes a correctness/availability difference; it cannot establish a DXMT/DXVK
speed ratio because DXVK cannot create a device on either tested ICD.

| Windows API | Recommended long-term route | Current decision / gate |
|---|---|---|
| D3D1–7 / DirectDraw | WineD3D Vulkan, after qualifying its PE dependencies and old fixed-function behavior | Future experiment; no verified backend in Raven on macOS |
| D3D8/9 | Upstream DXVK PE → Raven Vulkan bridge → KosmicKrisp → Metal, once its mandatory features work | Blocked: pinned DXVK rejects KosmicKrisp; WineD3D Vulkan is a separate possible experiment, not an implemented fallback |
| D3D10/10.1/11 | DXMT PE + guest x86_64 unixlib → Metal | Keep default; existing IOSurface transport works. Tessellation remains broken. Qualify DXVK per program after driver gates pass |
| D3D12 | vkd3d-proton PE + compatible DXGI → host Vulkan bridge → KosmicKrisp; reassess DXMT D3D12 when released | Experimental candidates only; no working D3D12 backend established here |
| Desktop OpenGL / WGL | Mesa Zink, preferably PE WGL/Gallium → Raven Vulkan bridge → arm64 KosmicKrisp | Target, not an available GL 4.6 implementation on this driver: geometry/XFB and other requirements must pass first |
| Vulkan | Existing host bridge; qualify KosmicKrisp alongside MoltenVK | KosmicKrisp works for compute/rendering and real Cocoa presentation; retain MoltenVK default until contract/foreign-surface failures are resolved |

No new D3D or OpenGL support is claimed by adding an ICD. Do not fabricate Vulkan capability bits, override Mesa GL versions,
or remove DXVK's checks to make a driver appear compatible. Keep known failures strict.

## KosmicKrisp versus MoltenVK

LunarG's [SDK announcement](https://www.lunarg.com/lunarg-releases-vulkan-sdk-1-4-363-0/) is dated **2026-09-28**
(the text says released September 29). It announces Vulkan 1.4 conformance. Its
[product page](https://www.lunarg.com/products/kosmickrisp/) (accessed 2026-10-25) describes an MIT-licensed Mesa driver
using Metal 4, requiring Apple silicon and macOS 26+. Intel Macs are not supported.

The SDK 1.4.363.0 driver was extracted outside the checkout without executing installation or accepting a license dialog.
The official [SDK file catalog](https://vulkan.lunarg.com/sdk/files.json) supplied the archive size and SHA-256.
Its [build config](https://sdk.lunarg.com/sdk/download/1.4.363.0/mac/config.json) identifies the source as
`https://gitlab.freedesktop.org/aitor/mesa.git`, branch `vulkan-sdk-1.4.363.0`.
The binary reports Mesa `26.2.99`, `vulkan-sdk-1.4.363 (git-269a955c89)`, device API `1.4.363`, conformance `1.4.3.2`.
This is the SDK's particular build, not a claim about all Mesa main revisions.

Measured by native `vulkaninfo`, with only this ICD enabled:

| Capability | Installed MoltenVK 1.2.11 (prior verification in VULKAN.md) | KosmicKrisp SDK 1.4.363.0 (this run) |
|---|---|---|
| Device Vulkan API | 1.2.296 | 1.4.363 |
| Geometry shaders | false | **false** |
| Tessellation | not sufficient for DXVK | true |
| Non-solid fill | not a workaround for missing features | **false** |
| Descriptor indexing | insufficient to establish modern D3D support | update-after-bind, partially bound, variable count and runtime arrays true; 1,048,576 set update-after-bind samplers/images/buffers |
| Robustness2 | robustBufferAccess2/nullDescriptor false | robustBufferAccess2, robustImageAccess2, nullDescriptor true; EXT and KHR robustness2 advertised |
| Transform feedback | no usable D3D stream-output path established | **VK_EXT_transform_feedback absent** (property fields do not imply extension support) |
| Depth clip, maintenance4/5/6, load-store-op-none, zero-init workgroup | absent in installed driver | advertised (including applicable core promotions) |
| Mutable descriptor type / descriptor buffer | absent | mutable descriptor type present; descriptor buffer absent |
| Custom border color / image-view min LOD | do not assume | not advertised by default; source gates them behind experimental flags |
| Surface support | Metal and headless in tested setup | Metal surface present; headless surface absent |

The driver is **arm64-only** (`file`/`otool`), depending only on macOS system libraries/frameworks. Raven's host bridge can use
it for either x64 or x86 Windows callers. It cannot be dlopened in Raven's x86_64 Rosetta agent. An x86_64 source build and
Rosetta compatibility were not verified; Apple-silicon GPU support does not imply an x86_64 binary exists.

[Mesa build instructions](https://docs.mesa3d.org/drivers/kosmickrisp.html) (accessed 2026-10-25) list Xcode, Meson 1.9.1+,
LLVM 20.1.8+, libclc, SPIRV-LLVM-Translator, SPIRV-Tools and Python build dependencies. A lean source configuration is:

```sh
meson setup build /external/mesa --buildtype=release -Dplatforms=macos \
  -Dvulkan-drivers=kosmickrisp -Dgallium-drivers= -Dopengl=false -Dzstd=disabled --prefer-static
ninja -C build -j6
```

Use the SDK pin for reproducible experiments. A source build was unnecessary for this verification.
The pinned [shader compiler source](https://gitlab.freedesktop.org/aitor/mesa/-/blob/vulkan-sdk-1.4.363.0/src/kosmickrisp/vulkan/kk_shader.c)
(accessed 2026-10-25) lowers vertex/tessellation-control stages to compute and tessellation-evaluation to a vertex stage, using
Mesa poly emulation. This is a distinct compiler path from DXMT. Geometry emulation is not exposed by the tested driver; comments
about geometry or populated geometry limits are not proof of support. No successful Raven tessellation draw was established here.

[Mesa licensing](https://docs.mesa3d.org/license.html) requires per-component/source-file notices; most code is MIT, not every
file. Driver source has MIT SPDX identifiers. Retain dependency notices when packaging a rebuilt binary; this work provisions
locally and does not redistribute a driver, SDK installer, or Apple software.

Modern MoltenVK must not be confused with the installed 1.2.11 baseline: SDK 1.4.363.0 includes MoltenVK 1.4.2.
Its [runtime guide](https://github.com/KhronosGroup/MoltenVK/blob/v1.4.2/Docs/MoltenVK_Runtime_UserGuide.md)
still documents portability limitations. Its [pinned feature implementation](https://github.com/KhronosGroup/MoltenVK/blob/v1.4.2/MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm)
(accessed 2026-10-25) sets non-solid fill true, but explicitly returns false for `robustBufferAccess2` and `nullDescriptor`.
Thus its advertised robustness2 extension alone still does not satisfy DXVK/vkd3d requirements. This is source inspection,
not a new hardware result. Only installed 1.2.11 and isolated KosmicKrisp were run here; no execution result for 1.4.2 is inferred.

## Direct3D choices and requirements

[DXVK 3.1.1's actual feature checks](https://github.com/doitsujin/dxvk/blob/v3.1.1/src/dxvk/dxvk_device_info.cpp)
(accessed 2026-10-25) require Vulkan 1.3 and numerous core/extension features, including non-solid fill, geometry shaders,
cull distance, Vulkan memory model and robustness2. These checks apply to its common adapter even for D3D9.
Transform feedback supports D3D stream output; descriptor-buffer availability is not its universal minimum requirement.
Its D3D8 wrapper uses D3D9. DXVK is zlib-licensed; retain its notice and the pinned package provenance in `backends.json`.

[vkd3d-proton 3.0.1's driver requirements](https://github.com/HansKristian-Work/vkd3d-proton/blob/v3.0.1/README.md#drivers)
(accessed 2026-10-25) include Vulkan 1.3, essentially all descriptor-indexing features, at least a million update-after-bind
descriptors (except uniform buffers), robustness2, push descriptors, mirror clamp and shader draw parameters. Mutable descriptor
type and descriptor buffer are recommended, not mandatory. KosmicKrisp meets substantially more of this floor than installed
MoltenVK, but that is not a D3D12 conformance result. The pinned vkd3d package shares DXVK's DXGI, so adapter rejection can happen
before vkd3d feature validation. vkd3d-proton is LGPL-2.1-or-later; preserve source/relinking obligations for distribution.

The older [DXVK-macOS 1.10.3 release](https://github.com/Gcenx/DXVK-macOS/releases/tag/v1.10.3)
(accessed 2026-10-25) explicitly supports **D3D10/11 only**, with Wine/CrossOver dependencies. It is not evidence that current
upstream DXVK, D3D9, or D3D12 works on MoltenVK. [CrossOver 26's settings](https://support.codeweavers.com/en_US/advanced-settings-in-crossover-mac-26)
(accessed 2026-10-25) retain multiple backends and a per-game database. [Whisky's repository](https://github.com/Whisky-App/Whisky)
(accessed 2026-10-25) says it is no longer actively maintained. Neither is Raven-compatible merely by copying its native driver.

[DXMT's current description](https://github.com/3Shain/dxmt) remains D3D10/11. Its
[runtime specifications](https://github.com/3Shain/dxmt/wiki/Device-System-Runtime-Specifications)
(accessed 2026-10-25) explicitly support both 32- and 64-bit Windows programs in Wine. Raven's missing WoW64 unix-call table is
therefore an integration gap, not a DXMT project limitation. No authoritative D3D9 implementation or committed roadmap was found.
D3D12 **is actively being implemented**: [part 10, opened 2026-10-22](https://github.com/3Shain/dxmt/pull/249),
with an explicit author warning, “DO NOT USE”; `enable_d3d12` defaults false in
[meson.options](https://github.com/3Shain/dxmt/blob/main/meson.options). Do not treat this as released D3D12 support.
The pinned [v0.80 license](https://github.com/3Shain/dxmt/blob/v0.80/LICENSE) is MIT; newer
[main is LGPL-2.1-or-later](https://github.com/3Shain/dxmt/blob/main/LICENSE). See `native/upstream/dxmt` for linked dependencies.

Raven's `native/nt/x86_d3d9.cpp` only creates counted stub COM objects for `Direct3DCreate9`/`CreateDevice`;
QueryInterface fails and remaining methods return E_NOTIMPL. It cannot render. Real x86 DXVK DLLs would bypass this stub and
use Raven's 32-bit Vulkan bridge. This avoids a WoW64 native unixlib, but the **same rejected adapter** blocks it today.
There is no measured basis to promise earlier WoW64 D3D9/11 through DXVK than through DXMT table support.

## OpenGL and WineD3D

[Zink's requirements](https://docs.mesa3d.org/drivers/zink.html) (accessed 2026-10-25) make GL level depend on Vulkan features.
GL 3.0 needs transform feedback; GL 3.2 adds geometry shaders; higher versions add tessellation and other limits/features.
The docs call macOS support experimental. KosmicKrisp's missing XFB/geometry (and non-solid fill) prevent claiming GL 4.6,
including compatibility profile. Zink is the candidate among these stacks for full desktop GL, but **none of the tested Mac
stacks establishes accelerated GL 4.6 compatibility for WGL**. A Mesa version override would hide bugs rather than implement GL.

Apple CGL/OpenGL tops out at 4.1 **core**, with a separate legacy profile, not a 4.6 compatibility context. Its
[profile documentation](https://developer.apple.com/library/archive/documentation/GraphicsImaging/Conceptual/OpenGL-MacProgGuide/opengl_pixelformats/opengl_pixelformats.html)
(accessed 2026-10-25) also documents deprecation since macOS 10.14. A CGL fallback could serve limited old programs, but would
add a second host context/presentation integration and not satisfy the long-term compatibility goal.
[ANGLE's renderer matrix](https://chromium.googlesource.com/angle/angle/+/refs/heads/main/README.md)
(accessed 2026-10-25) is OpenGL **ES**/EGL translation, including Metal. It is not desktop GL/WGL or a compatibility profile.
Raven's existing GL bridge binds `EGL_OPENGL_API`, so ANGLE is not a drop-in backend.

[Wine 11.0's announcement](https://github.com/wine-mirror/wine/blob/wine-11.0/ANNOUNCE.md)
(January 2026; accessed 2026-10-25) lists new Vulkan-renderer fixed-function/Shader Model 1–3 support, but explicitly says it
is not at parity with WineD3D's GL renderer. WineD3D serves old DirectDraw/D3D APIs through GL or Vulkan; D3D8/9 are reasonable
future experiments. A Vulkan-only PE build could use Raven's bridge, but its opengl32, user32/gdi32/advapi32 and vkd3d-shader
imports/runtime assumptions must be qualified. Its GL route first requires working WGL. It is Wine LGPL-2.1-or-later code,
not an existing independently provisioned Raven component. No WineD3D device or rendering success was verified here.

## Apple D3DMetal / GPTK

Apple's [current GPTK page](https://developer.apple.com/games/game-porting-toolkit/) (accessed 2026-10-25) describes GPTK 4,
an evaluation environment with Metal 4, HDR, sparse resources and MetalFX-related features. CrossOver documents D3D11/12
support. This is not D3D9 or desktop GL, and not an arm64 host bridge with a stable public Raven ABI.

The publicly available text attributed to Apple's GPTK SLA (EA18380, 2023-08-17) grants internal game development/testing/
evaluation on Apple products, and noncommercial distribution with conditions. Sections 2A/2C permit separately distributing
the **whole framework** or individual redist components, with notices and restrictions; the blanket claim “never redistributable”
is inaccurate. Section 3 restricts copies supplied for evaluation. This text was inspected from a
[third-party copy](https://raw.githubusercontent.com/Mon-ius/steam-macos-setup-skill/master/vendor/gptk-redist/Apple-GPTK-SLA.txt),
not accepted, and **is not verified as the binding license of GPTK 4 beta 3**. Apple's
[agreements page](https://developer.apple.com/support/terms/) says accepted English agreements control.

A user-installed copy avoids Raven distributing the framework, but does **not** remove its use restrictions or grant Raven
permission for arbitrary applications/commercial runtime use. A narrowly authorized game-evaluation use may fit the stated grant;
that is conditional, not a legal clearance for Raven. No current authenticated package/license was downloaded or accepted, no
Apple binary was used, and no general-use integration is recommended without checking the actual user's applicable agreement.
CrossOver's negotiated permissions cannot be assumed to transfer to Raven.

## Architecture and backend selection

DXMT's PE code and x86_64 Mach-O unixlib share the translated guest's pointers and use the tested Win64→SysV gate. The native
adapter sends completed IOSurface rights to the arm64 host, which presents them in an ordinary Cocoa child layer. Raven would
need pinned WoW64 argument conversion/table dispatch for 32-bit DXMT; an arm64 DXMT library cannot be loaded into this agent.

DXVK/vkd3d PE DLLs execute in the guest and use `vulkan-1.dll`; marshaled commands execute in the arm64 controller and driver.
Win32 Vulkan surfaces map to the existing CAMetalLayer, with foreign-window readback only where supported. A driver swap does
not eliminate bridge overhead or expand its marshaling coverage. Zink **PE** would reuse this route for WGL and both guest widths,
provided its Windows winsys/imports work. A host arm64 Zink EGL build instead reuses Raven's existing GL command bridge; it needs
a supported macOS EGL winsys and Cocoa drawable integration. Neither is achieved by loading an x86_64 Mesa unixlib into the host.

`native/graphics/backends.json` pins **provisioning inputs**; it is not read as a runtime per-program routing database.
Currently program-local DLLs take precedence, followed on Darwin by configured DXMT modules, then general D3D directory lookup.
`RAVEN_D3D_DIRECTORY` alone does not override configured DXMT for D3D11. A future explicit per-program backend setting must select
a coherent DLL family/DXGI and native root; it must not mix DXMT DXGI and DXVK D3D11. Keep DXMT default today; a future DXVK fallback
requires per-program correctness and CPU/present measurements before promotion. No automated fallback to a known failing driver.

## Hands-on evidence

The first isolated run used the existing loader 1.4.309 and SDK 1.4.363.0's arm64 ICD. The driver pin and SHA-256s are recorded in
`native/upstream/kosmickrisp/README.md`. No system installation, shared component replacement, debugger or capability override.
Build: supplied configuration, `cmake --build build -j6`. Tests always serial; focused checks, not a full baseline comparison.

Nine existing Vulkan checks: **6 passed, 3 failed** in 19.53 s:

- PASS: shared GPU memory fallback, Vulkan CLI, extensions x64/x32, x86 compute, Cocoa presentation.
- Cocoa screenshots matched all four quadrant interiors at 256×192 and after resize to 320×200.
- FAIL: `native_vulkan_contract`: `guest memory properties differ from host`. Its test compares complete fixed arrays with memcmp,
  including inactive entries/padding. This is a test/driver-output hypothesis, not a diagnosed bridge defect; no expectation relaxed.
- FAIL: foreign presentation x64/x32: `Foreign presentation returned 2`, no frame. KosmicKrisp does not advertise headless surfaces;
  the exact failure site remains to be isolated. Real Cocoa Metal WSI succeeds.

D3D11 comparison (final locked run): same PE fixtures (compiled from `testdata/d3d11`, plus the Darwin pacing fixture), default scheduler, one serial
sample per backend, separate fresh prefixes and local DXVK DLLs. An earlier exploratory sample is retained separately. Host+guest CPU is the `RUSAGE_CHILDREN` user+system delta around
only the runtime invocation (including its reaped agent); wall time includes startup and any shader-cache effects. Other delegates
were active on the host, so these are observations, not stable performance benchmarks. `OK` alone was not accepted for pixel tests.

| Fixture | DXMT correctness | DXMT wall / CPU seconds | DXVK + KosmicKrisp | DXVK wall / CPU seconds (failed startup only) |
|---|---|---|---|---|
| triangle | pixel reference passed | 3.115 / 3.960 | device rejected | 3.176 / 3.998 |
| deferred | pixel reference passed | 3.037 / 3.835 | device rejected | 3.146 / 3.908 |
| geometry | pixel reference passed | 3.040 / 3.800 | device rejected | 3.072 / 3.807 |
| tessellation | **pixel reference failed, clear only** | 3.045 / 3.793 | device rejected | 3.090 / 3.818 |
| textured | pixel reference passed | 3.024 / 3.782 | device rejected | 3.085 / 3.825 |
| compute | all 256 values matched | 3.094 / 3.883 | device rejected | 3.357 / 4.152 |
| MSAA | pixel reference passed | 3.052 / 3.804 | device rejected | 3.201 / 3.923 |
| present rate | 2,000 timed presents / 6.909896 s = **289.4 presents/s** | 10.892 / 8.712 | device rejected; no rate | 4.024 / 4.726 |
| interval-4 pacing | 16 completed presents / **1.024427 s** | 5.204 / 5.259 | device rejected; no pacing | 3.607 / 4.404 |

All nine DXVK logs identify v3.1.1 and KosmicKrisp, then report `Device does not support required feature 'fillModeNonSolid'`.
Device HRESULT is `80004005` (pacing exits before writing status). Geometry is independently absent and also mandatory. DXVK does
**not** fix tessellation in this setup: it cannot reach the shader. Comparing its failed-startup CPU with rendered DXMT CPU as a
speedup would be invalid. Present rate is submission-loop throughput, not independently measured display FPS or vblank accuracy.

The local raw logs/results are under `build/native/graphics-research/`, and Vulkan evidence under
`build/native/vulkan-cocoa-present/` plus `/tmp/raven-kosmickrisp-tests.log` and `/tmp/raven-kosmickrisp-vulkaninfo.txt`.
Committed evidence records below are small summaries; screenshots, prefixes and third-party payloads stay outside git.

## Next gates

1. Keep MoltenVK and DXMT defaults. Expose the pinned KosmicKrisp ICD only by explicit test/runtime selection.
2. Diagnose memory-array comparison and headless/foreign WSI failures without relaxing pixel results; qualify a current MoltenVK
   separately if it becomes a candidate. Use Metal WSI for actual Cocoa windows.
3. Track KosmicKrisp non-solid-fill, geometry and transform-feedback implementation. Re-run strict DXVK D3D9/x86/D3D11 fixtures
   after those land. No ABI or driver patch is justified by the current rejection.
4. Independently test WineD3D Vulkan PE as a narrower D3D8/9 alternative, or DXMT WoW64 table support using the already-supported
   upstream 32-bit driver. Neither depends on pretending DXVK currently works.
5. Run Zink's requirement checker and then real WGL compatibility/core context, legacy drawing, buffer mapping, sharing, readback,
   resize and on-screen tests. Only then pin/provision Mesa GL. Native CGL/ANGLE cannot satisfy the requested desktop GL target.
6. Revisit vkd3d with a qualified DXGI/driver combination and DXMT's released D3D12 status; require actual device/draw/readback,
   not an API-version inference. Do not add an Apple backend until its precise applicable license and guest ABI are resolved.

Machine-readable first-run timings: [comparison-2026-10-25.json](graphics/comparison-2026-10-25.json).
Measured driver features/extensions: [capabilities-2026-10-25.json](graphics/capabilities-2026-10-25.json).
These are single samples with the limitations above; failed startup timings are not graphics performance results.

## Provisioning and reproducing the comparison

Import an SDK's already-obtained `macOS` files without installing globally:

```sh
python3 scripts/provision-kosmickrisp.py --sdk /external/sdk/macOS \
  --destination /Users/yount/Documents/Projects/raven-components/kosmickrisp-1.4.363.0
cmake -S . -B build \
  -DRAVEN_KOSMICKRISP_ROOT=/Users/yount/Documents/Projects/raven-components/kosmickrisp-1.4.363.0 \
  -DRAVEN_DARWIN_DXVK_TESTS=ON -DRAVEN_DARWIN_PRESENT_TEST=ON
cmake --build build -j6
/tmp/raven-ctest-lock.sh gfx ctest --test-dir build -j1 --timeout 120 \
  -R '^darwin_(dxvk_|dxmt_(triangle|deferred|geometry|tessellation|textured|compute|msaa|present_rate|pacing)$)' \
  --output-on-failure
```

The ordinary build inputs/component roots from README/DXMT.md remain required. `RAVEN_KOSMICKRISP_ROOT` selects **tests** and
validates the two pinned files at configure time; it does not change the installed runtime's default ICD. For an explicit manual
runtime experiment set `VK_DRIVER_FILES=<root>/share/vulkan/icd.d/libkosmickrisp_icd.json`. `RAVEN_VULKAN_TEST_ICD` continues to
support an arbitrary explicit ICD when the KosmicKrisp root option is empty. Its selection now also reaches existing native
D3D8/9/11/12 tests rather than only Vulkan-named tests. No bridge or upstream runtime code was changed.

The opt-in DXVK tests are strict and currently **expected to fail**; they are not successful-support or expected-failure tests.
They reuse the same D3D11 executables and pixel/compute checks as DXMT, including the present-rate and interval-four fixtures.
Additional PE32 D3D11 and PE32/PE64 D3D9 tests exercise real DLL loading and readback. The D3D9 test checks completed presents
and GPU first/last-frame pixels, not captured screen pixels. The common runner records `metrics.json`, stdout and stderr beside
each isolated prefix even when device creation fails. CPU accounting excludes Python pixel-checker work.

The opt-in test flag defaults off, and no unsupported driver is silently substituted for DXMT or MoltenVK. Disable the root
option and explicitly choose the MoltenVK test ICD to restore that test configuration.


## Final verification and implementation boundary

The final shared-lock batch ran **31 tests in 104.34 seconds: 14 passed, 17 failed**. Vulkan repeated its six passes and three
failures. DXMT passed eight of the nine selected D3D11 tests; tessellation failed its strict pixel check. All nine corresponding
x64 DXVK D3D11 tests failed adapter creation. The existing vkd3d-proton D3D12 device test also failed (`hr=80004005`) through
DXVK DXGI's non-solid-fill rejection. No D3D12 device/command execution is claimed.

The new x64 D3D9 test reaches DXVK, then adapter rejection and an unhandled guest C++ exception (`20474343`). The x86 D3D9 test
exits 3 after the same rejection; x86 D3D11 returns `FAIL device hr=80004005`. Their Windows-side DXVK logs identify v3.1.1,
KosmicKrisp and missing `fillModeNonSolid`. The first runner checked stderr only and therefore reported missing backend identity
for these two tests. It now collects PE32 log files too. The updated runner was verified against both saved real PE32 outputs, mocking only the
native subprocess: it retained backend identity, the feature rejection and failure exit codes. No additional native run or new
performance sample is claimed by this diagnostic replay. The redundant queued hardware reruns were canceled before acquiring
the shared test lock. These results establish
that real PE32 DXVK loads and reaches Raven's Vulkan bridge, **not working WoW64 Direct3D**.

Final timings and test outcomes: [verification-2026-10-25.json](graphics/verification-2026-10-25.json). Complete local JUnit and
logs are `/tmp/raven-graphics-final.{xml,log}`; diagnostic replay results are `build/native/graphics-research/log-replay.json`. Each common
runner directory contains `metrics.json`; these measurements exclude checker CPU. No speed ratio is computed against failed DXVK.

The implemented milestone is the reproducible driver qualification path: pinned external ICD import, configure-time checksum
validation, consistent ICD selection for graphics tests, and opt-in strict paired D3D11 / PE32 / D3D9 tests with CPU/wall metrics.
Provisioning was checked twice for idempotent verification; invalid SDK, in-tree/overlapping destinations and unknown existing roots
were rejected without modifying their contents. The supplied build completed with `-j6`; Python compilation and `git diff --check`
passed. Upstream runtime/bridge files and `cocoa.mm` have **zero changed lines**. No new D3D9/OpenGL rendering backend was enabled: those
paths stopped at the measured capability gates. Defaults remain DXMT and MoltenVK. No Apple software was used or redistributed.
