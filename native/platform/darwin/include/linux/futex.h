// The runtime waits on words in MAP_SHARED files that other processes change, through syscall(SYS_futex, ...). macOS has the same
// cross-process wait on an address in os_sync_wait_on_address; this overload of syscall catches exactly those calls.
#pragma once
#include <stdint.h>
#include <time.h>

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_WAIT_PRIVATE (FUTEX_WAIT | FUTEX_PRIVATE_FLAG)
#define FUTEX_WAKE_PRIVATE (FUTEX_WAKE | FUTEX_PRIVATE_FLAG)
#ifndef SYS_futex
#define SYS_futex 0x7f00f001
#endif

// futex_waitv (Linux 5.16) has no macOS counterpart: the call answers ENOSYS, as an older Linux kernel does, and the runtime waits on
// several shared words through its scheduler instead.
#define FUTEX_32 2
#ifndef SYS_futex_waitv
#define SYS_futex_waitv 0x7f00f002
#endif
struct futex_waitv {
	uint64_t val;
	uint64_t uaddr;
	uint32_t flags;
	uint32_t __reserved;
};

#ifdef __cplusplus
#include <atomic>
#include <cerrno>
#include <cstddef>

template <class... Rest>
inline long syscall(int, struct futex_waitv *, Rest...)
{
	errno = ENOSYS;
	return -1;
}

template <class... Rest>
inline long syscall(int, std::nullptr_t, Rest...)
{
	errno = ENOSYS;
	return -1;
}

extern "C" long raven_darwin_futex(void *word, int operation, uint32_t value, const timespec *timeout);

template <class Word, class Value, class... Rest>
inline long syscall(int number, std::atomic<Word> *word, int operation, Value value, const timespec *timeout = nullptr, Rest...)
{
	static_assert(sizeof(Word) == 4, "futex words are 32 bits");
	(void)number;
	return raven_darwin_futex(word, operation, uint32_t(value), timeout);
}
#endif
