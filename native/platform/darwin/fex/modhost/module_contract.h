// SPDX-License-Identifier: MIT
#pragma once
// Qualified patch series: 0001-0019 (M4 boundary gating, M5 fault repair, private host TEB ABI v2).
namespace RavenContract {
constexpr uint32_t process_init_rva = 0x6e58;
constexpr uint32_t process_init_instruction = 0xb40001c0;
// Reverified against integration-review xtajit.dll export and entry bytes.
constexpr uint32_t wow64_process_init_rva = 0x799c;
constexpr uint32_t wow64_process_init_instruction = 0xf81f0ffe;
} // namespace RavenContract
