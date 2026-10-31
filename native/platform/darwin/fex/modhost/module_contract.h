// SPDX-License-Identifier: MIT
#pragma once
// Qualified patch series: 0001-0018 (M4 boundary gating, M5 fault repair, private host TEB ABI v2).
namespace RavenContract {
constexpr uint32_t process_init_rva = 0x6c34;
constexpr uint32_t process_init_instruction = 0xb40001c0;
constexpr uint32_t wow64_process_init_rva = 0x7850;
constexpr uint32_t wow64_process_init_instruction = 0xf81f0ffe;
} // namespace RavenContract
