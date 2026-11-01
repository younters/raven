// SPDX-License-Identifier: MIT
#pragma once
// Qualified patch series: 0001-0019 (M4 boundary gating, M5 fault repair, private host TEB ABI v2).
namespace RavenContract {
// Multiblock enabled (0028 removed, 0041 call-ret invalidation): RavenProcessInit moved.
constexpr uint32_t process_init_rva = 0x705c;
constexpr uint32_t process_init_instruction = 0xb40001c0;
// Reverified against 0041 xtajit.dll export and entry bytes (WoW64 RVA unchanged).
constexpr uint32_t wow64_process_init_rva = 0x79cc;
constexpr uint32_t wow64_process_init_instruction = 0xf81f0ffe;
} // namespace RavenContract
