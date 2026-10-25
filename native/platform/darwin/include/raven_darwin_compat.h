// Included into every host translation unit of the macOS build (see toolchain.cmake), ahead of any system header.
#pragma once

// IPV6_PKTINFO, IPV6_HOPLIMIT and the other RFC 3542 socket options, which Linux exposes by default.
#define __APPLE_USE_RFC_3542 1

// ucontext_t has room for its machine context only under _XOPEN_SOURCE, which the thread scheduler's coroutines need (see ucontext.h).
// A file that met the type first through another header would lay out every structure holding one differently, so every file defines it
// here, with the room, before anything else can. sys/cdefs.h goes first so the rest of the system headers see the usual environment.
#include <sys/cdefs.h>
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#include <sys/_types/_ucontext.h>
#undef _XOPEN_SOURCE
#else
#include <sys/_types/_ucontext.h>
#endif
