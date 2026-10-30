// Observe main's startup seam at normal process exit without a production flag.
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>

static void report_limit(void)
{
	struct rlimit limits;
	if (!getrlimit(RLIMIT_NOFILE, &limits))
		printf("fd-budget-observed %llu %llu\n", (unsigned long long)limits.rlim_cur, (unsigned long long)limits.rlim_max);
}
__attribute__((constructor)) static void observe_startup(void)
{
	atexit(report_limit);
}
