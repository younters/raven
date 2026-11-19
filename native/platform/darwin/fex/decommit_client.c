// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
int raven_entry(void)
{
	BYTE *base = VirtualAlloc(NULL, 65536, MEM_RESERVE, PAGE_NOACCESS);
	DWORD old;
	if (!base) return 1;
	if (!VirtualFree(base, 65536, MEM_DECOMMIT)) return 2;
	if (!VirtualAlloc(base + 4096, 8192, MEM_COMMIT, PAGE_READWRITE)) return 3;
	base[4096] = 17;
	base[8192] = 42;
	if (!VirtualProtect(base + 4096, 4096, PAGE_READONLY, &old)) return 4;
	if (!VirtualProtect(base + 8192, 4096, PAGE_NOACCESS, &old)) return 5;
	if (!VirtualFree(base, 65536, MEM_DECOMMIT)) return 6;
	if (!VirtualFree(base, 65536, MEM_DECOMMIT)) return 7;
	MEMORY_BASIC_INFORMATION info;
	if (!VirtualQuery(base + 4096, &info, sizeof(info)) || info.State != MEM_RESERVE) return 8;
	if (!VirtualAlloc(base, 65536, MEM_COMMIT, PAGE_READWRITE)) return 9;
	for (unsigned i = 0; i < 65536; ++i) if (base[i]) return 10;
    base[0]=11; base[4096]=22; base[8192]=33; base[12288]=44;
    if (!VirtualFree(base+4096,4096,MEM_DECOMMIT)) return 12;
    if (base[0]!=11 || base[8192]!=33 || base[12288]!=44) return 13;
    if (!VirtualAlloc(base+4096,4096,MEM_COMMIT,PAGE_READONLY)) return 14;
    if (base[4096]) return 15;
    if (!VirtualQuery(base+4096,&info,sizeof info) || info.Protect!=PAGE_READONLY) return 16;
    if (!VirtualProtect(base+4096,4096,PAGE_READWRITE,&old) || old!=PAGE_READONLY) return 17;
    base[4096]=55;
    return VirtualFree(base, 0, MEM_RELEASE) ? 0 : 11;
}
