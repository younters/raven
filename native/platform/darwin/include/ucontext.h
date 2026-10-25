// The host coroutines of the thread scheduler use getcontext/makecontext/swapcontext, which macOS still provides but declares only
// under _XOPEN_SOURCE; defining that for every file would hide the Darwin interfaces the rest of the runtime uses.
#pragma once
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#define RAVEN_DARWIN_XOPEN_SOURCE
#endif
#include_next <ucontext.h>
#ifdef RAVEN_DARWIN_XOPEN_SOURCE
#undef _XOPEN_SOURCE
#undef RAVEN_DARWIN_XOPEN_SOURCE
#endif
