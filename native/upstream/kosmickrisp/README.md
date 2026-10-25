# KosmicKrisp for the Darwin host

Locally verified 2026-10-25. Upstream: https://gitlab.freedesktop.org/mesa/mesa.
SDK build source: https://gitlab.freedesktop.org/aitor/mesa.git, branch `vulkan-sdk-1.4.363.0`,
as recorded in https://sdk.lunarg.com/sdk/download/1.4.363.0/mac/config.json.
Binary driverInfo: `vulkan-sdk-1.4.363 (git-269a955c89)`; Mesa driverVersion 26.2.99.
The abbreviated source identity is what the binary reports; the immutable binary hashes below are the provisioning pins.

Official SDK archive: https://sdk.lunarg.com/sdk/download/1.4.363.0/mac/vulkansdk-macos-1.4.363.0.zip

- Release: 2026-09-29, SDK 1.4.363.0.
- Size: 396,088,200 bytes.
- SHA-256: `163431e0203c8215286a17cf393643ce6b951f77d1b4f74ab30821a1f0beb09b`.
- Hash/size checked against https://vulkan.lunarg.com/sdk/files.json.
- `macOS/lib/libvulkan_kosmickrisp.dylib`: `52d7b68364e19d744882533d663d69b898bfd294f4248b3313785ad801e23e58`.
- All selected file hashes: [kosmickrisp.json](../../platform/darwin/graphics/kosmickrisp.json).

Only the arm64 Mach-O ICD and its relative-path JSON manifest are provisioned, unchanged. No loader, tools, installer,
Apple framework, driver implementation source or shader compiler source is vendored. The ICD links only system frameworks
and dylibs (Metal, QuartzCore, Foundation, CoreFoundation, libSystem, libc++, libobjc, libexpat and libz).
It belongs in the arm64 controller; it cannot be loaded into the x86_64 Rosetta guest agent.

`scripts/provision-kosmickrisp.py` imports these two checksum-pinned files from an SDK already obtained by the user, into a
new external component root. It never runs an installer, accepts agreements, writes system locations or alters an existing
component root. SDK paths may point at an installation or a manually extracted `macOS` directory. Getting the SDK and checking
any applicable installer terms remains the user's responsibility. No third-party account or agreement is needed for running
the already-extracted open-source ICD in the verification described here.

During research the SDK zip was downloaded to `raven-components/kosmickrisp-sdk-1.4.363.0/`. Its installer.dat contains a 7z
payload at byte 2,021,742, length 355,496,067. Only the ICD, manifest and selected development tools were extracted using 7zz;
the installer was not run except for `--help`. This payload layout is an observation, not a supported installer API. The
provisioner deliberately imports known SDK files instead of depending on Qt installer's private archive layout.

KosmicKrisp source declares SPDX MIT, including copyrights © 2022 Collabora Ltd. and Red Hat Inc., © 2025 LunarG, Inc., and
© 2025 Google LLC in `src/kosmickrisp/vulkan/kk_physical_device.c`. The upstream license template is retained in `MIT.LICENSE`:
https://gitlab.freedesktop.org/aitor/mesa/-/raw/vulkan-sdk-1.4.363.0/licenses/MIT.
Mesa's licensing guide (https://docs.mesa3d.org/license.html) requires checking individual component notices; this MIT template
is not an assertion that every compiled dependency is MIT. This provisioner is for private local experiments, not a binary
redistribution package. Any release/bundle must retain the complete relevant source/dependency notices and fulfill their terms.

The Vulkan 1.4 API/conformance claim does not imply DXVK or full desktop OpenGL compatibility. See
[GRAPHICS.md](../../platform/darwin/GRAPHICS.md) for the measured feature gaps and failed fixtures. MoltenVK remains the default.
