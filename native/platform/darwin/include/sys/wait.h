#pragma once
#include_next <sys/wait.h>

// Linux's waitpid flag for any child, cloned threads included. Only upstream's ptrace host threads use it, which the macOS engines leave
// off (threaded_host); macOS has no such children.
#ifndef __WALL
#define __WALL 0x40000000
#endif
