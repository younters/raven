#pragma once
#include_next <sched.h>
#include <string.h>
#include <sys/types.h>

// CPU affinity: macOS does not let a process pin itself, so every processor is in the set.
typedef struct { unsigned long long bits[16]; } cpu_set_t;
#define CPU_SETSIZE 1024
#define CPU_ZERO(set) memset((set), 0, sizeof(cpu_set_t))
#define CPU_SET(cpu, set) ((set)->bits[(cpu) / 64] |= 1ULL << ((cpu) % 64))
#define CPU_CLR(cpu, set) ((set)->bits[(cpu) / 64] &= ~(1ULL << ((cpu) % 64)))
#define CPU_ISSET(cpu, set) (((set)->bits[(cpu) / 64] >> ((cpu) % 64)) & 1)
static inline int raven_darwin_cpu_count(const cpu_set_t *set)
{
	int count = 0;
	for (unsigned i = 0; i < 16; i++)
		count += __builtin_popcountll(set->bits[i]);
	return count;
}
#define CPU_COUNT(set) raven_darwin_cpu_count(set)

#ifdef __cplusplus
extern "C" {
#endif
int sched_getaffinity(pid_t pid, size_t size, cpu_set_t *set);
#ifdef __cplusplus
}
#endif
