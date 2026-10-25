// The runtime waits on words in MAP_SHARED files that other processes change, through syscall(SYS_futex, ...). macOS has the same
// cross-process wait on an address in os_sync_wait_on_address; this overload of syscall catches exactly those calls.
#pragma once
#include <stdint.h>

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128
#ifndef SYS_futex
#define SYS_futex 0x7f00f001
#endif

#ifdef __cplusplus
#include <atomic>

extern "C" long raven_darwin_futex(void *word, int operation, uint32_t value);

template <class Word, class Value, class... Rest>
inline long syscall(int number, std::atomic<Word> *word, int operation, Value value, Rest...)
{
	static_assert(sizeof(Word) == 4, "futex words are 32 bits");
	(void)number;
	return raven_darwin_futex(word, operation, uint32_t(value));
}
#endif
