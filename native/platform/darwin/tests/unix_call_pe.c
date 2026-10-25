#include "unix_call_shared.h"

typedef int32_t (*unix_gate)(uint64_t, uint32_t, void *);
extern uint32_t check_preserved(unix_gate, uint64_t, void *);

int mainCRTStartup(struct unix_call_probe *p)
{
	unix_gate call = (unix_gate)p->gate;
	uint64_t before, after;
	__asm__ volatile("movq %%gs:0x30, %0" : "=r"(before));
	p->preserved = check_preserved(call, p->handle, p);
	__asm__ volatile("movq %%gs:0x30, %0" : "=r"(after));
	if (!p->preserved || before != p->guest_teb || after != before || p->calls != 1)
		return 1;
	if ((uint32_t)call(p->handle, 1, p) != 0xc000000dU || (uint32_t)call(0, 0, p) != 0xc0000008U)
		return 2;
	for (int i = 0; i < 3; ++i)
		if (call(p->handle, 0, p) != 0x1234)
			return 3;
	return 0;
}
