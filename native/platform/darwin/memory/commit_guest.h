#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
namespace raven {
// Only cooperative Rosetta publishes the commit block. The scheduler defers preemption across
// its commit preludes, so SuspendThread cannot strand a runnable owner. A guest fault can still
// abandon a partial transaction. The host never recovers it by clearing an owner's lock.
inline constexpr size_t commit_guest_lock = 0xd08;
size_t commit_guest_jump(bool decommit);
std::vector<uint8_t> commit_guest_prelude(int32_t pointer, int32_t trap, bool decommit);
const std::vector<uint8_t> &commit_guest_nt_prelude(bool decommit);
class CommitBlockLock {
public:
	explicit CommitBlockLock(uint64_t *block) : word(block ? block + commit_guest_lock / 8 : nullptr), previous(owned), acquired(word && word!=owned)
	{
		if (acquired) {
			acquire(word);
			owned=word;
		}
	}
	~CommitBlockLock()
	{
		if (acquired) { owned=previous; __atomic_store_n(word, uint64_t(0), __ATOMIC_RELEASE); }
	}
	CommitBlockLock(const CommitBlockLock &) = delete;
private:
	// An abandoned section may contain a partial transaction; it cannot be recovered by unlocking.
	static void acquire(uint64_t *word)
	{
		const auto start = std::chrono::steady_clock::now();
		for (uint32_t tries = 1; __atomic_exchange_n(word, uint64_t(1), __ATOMIC_ACQUIRE); ++tries) {
			if (tries % 4096)
				continue;
			if (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100)) {
				std::this_thread::yield();
				continue;
			}
			std::fprintf(stderr, "raven: guest memory critical section did not finish (stopped, faulted or terminated owner); terminating process\n");
			std::abort();
		}
	}
	inline static thread_local uint64_t *owned = nullptr;
	uint64_t *word, *previous;
	bool acquired;
};
}
