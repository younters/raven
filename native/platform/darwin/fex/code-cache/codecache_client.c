// SPDX-License-Identifier: MIT
// Prints the library result, then runs JIT and self-modified image code whose
// marker immediates (0x5ec0de01, 0x5ec0de02) must never reach the code cache.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

// mov eax, imm32; ret. Kept as data so the markers never appear in this image's code.
static const volatile unsigned char jit_code[] = {0xb8, 0x01, 0xde, 0xc0, 0x5e, 0xc3};
static const volatile unsigned char patched_immediate[] = {0x02, 0xde, 0xc0, 0x5e};

uint32_t smc_target(void);
__asm__(".text\n.globl smc_target\n.p2align 4\nsmc_target:\n\tmovl $0x5ec0de00, %eax\n\tret\n");

static void print_hex(const char *label, uint32_t value)
{
	char line[64];
	unsigned n = 0;
	DWORD written;
	while (*label) line[n++] = *label++;
	for (int shift = 28; shift >= 0; shift -= 4) line[n++] = "0123456789abcdef"[(value >> shift) & 15];
	line[n++] = '\n';
	WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, n, &written, NULL);
}

int raven_entry(void)
{
	HMODULE library = LoadLibraryA("codecache_lib.dll");
	if (!library) return 1;
	uint32_t (*compute)(uint32_t) = (uint32_t (*)(uint32_t))GetProcAddress(library, "codecache_compute");
	if (!compute) return 2;
	print_hex("CODECACHE_RESULT=", compute(200000));

	unsigned char *jit = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	if (!jit) return 3;
	for (unsigned i = 0; i < sizeof(jit_code); ++i) jit[i] = jit_code[i];
	if (!FlushInstructionCache(GetCurrentProcess(), jit, sizeof(jit_code))) return 4;
	if (((uint32_t (*)(void))jit)() != 0x5ec0de01u) return 5;

	if (smc_target() != 0x5ec0de00u) return 6;
	unsigned char *target = (unsigned char *)smc_target;
	DWORD old;
	if (!VirtualProtect(target, 8, PAGE_EXECUTE_READWRITE, &old)) return 7;
	for (unsigned i = 0; i < sizeof(patched_immediate); ++i) target[1 + i] = patched_immediate[i];
	if (!VirtualProtect(target, 8, old, &old) || !FlushInstructionCache(GetCurrentProcess(), target, 8)) return 8;
	if (smc_target() != 0x5ec0de02u) return 9;
	print_hex("CODECACHE_SMC=", smc_target());
	return 0;
}
