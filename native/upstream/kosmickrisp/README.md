# KosmicKrisp for the Darwin host

Upstream source is https://gitlab.freedesktop.org/mesa/mesa.
SDK build source: https://gitlab.freedesktop.org/aitor/mesa.git, branch `vulkan-sdk-1.4.363.0`,
as recorded in https://sdk.lunarg.com/sdk/download/1.4.363.0/mac/config.json.
Binary driverInfo: `vulkan-sdk-1.4.363 (git-269a955c89)`; Mesa driverVersion 26.2.99.
The abbreviated source identity is what the binary reports; the immutable binary hashes below are the provisioning pins.

Official SDK archive: https://sdk.lunarg.com/sdk/download/1.4.363.0/mac/vulkansdk-macos-1.4.363.0.zip

- Size: 396,088,200 bytes.
- SHA-256: `163431e0203c8215286a17cf393643ce6b951f77d1b4f74ab30821a1f0beb09b`.
- Hash/size checked against https://vulkan.lunarg.com/sdk/files.json.
- `macOS/lib/libvulkan_kosmickrisp.dylib`: `52d7b68364e19d744882533d663d69b898bfd294f4248b3313785ad801e23e58`.
- All selected file hashes: [kosmickrisp.json](../../platform/darwin/graphics/kosmickrisp.json).

Only the arm64 Mach-O ICD and its relative-path JSON manifest are provisioned, unchanged. No loader, tools, installer,
Apple framework, driver implementation source or shader compiler source is vendored. The ICD links only system frameworks
and dylibs (Metal, QuartzCore, Foundation, CoreFoundation, libSystem, libc++, libobjc, libexpat and libz).
It belongs in the arm64 controller; it cannot be loaded into the x86_64 Rosetta guest agent.

[`provision-kosmickrisp.py`](../../platform/darwin/scripts/provision-kosmickrisp.py) imports these two checksum-pinned files from an SDK already obtained by the user, into a
new external component root. The SDK path may point at the installed or extracted `macOS` directory.

KosmicKrisp source declares SPDX MIT, including copyrights © 2022 Collabora Ltd. and Red Hat Inc., © 2025 LunarG, Inc., and
© 2025 Google LLC in `src/kosmickrisp/vulkan/kk_physical_device.c`. The upstream license template is retained in
[MIT.LICENSE](MIT.LICENSE). See [Mesa's licensing guide](https://docs.mesa3d.org/license.html) for component and dependency terms.

See [graphics on macOS](../../platform/darwin/GRAPHICS.md) for driver selection and tests.
