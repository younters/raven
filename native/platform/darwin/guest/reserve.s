// Holds the low 4 GiB, where Windows images and allocations go, so that dyld and malloc place nothing there. The agent gives the range
// back page by page as the host maps guest memory into it.
	.globl _raven_low_reserve
	.zerofill RAVEN_LOW,RAVEN_LOW,_raven_low_reserve,0xfffe0000
