#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <thread>
#include <vector>
namespace raven {
// Host and guest acquire the same word before touching commit windows or the log.
// Guest critical sections never trap, wait, or call the host; contention falls back unlocked.
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
	// A guest section ends at once unless its thread was stopped inside it, at a back edge or by a suspension. Only this host thread
	// can let that thread run again, so waiting for it would never end: after a while the section is taken to be stopped, and this
	// host thread goes on, as the guest cannot go on while it does.
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
			static std::atomic<bool> reported{false};
			if (!reported.exchange(true))
				std::fprintf(stderr, "raven: a stopped guest thread holds the memory lock; going on without it\n");
			return;
		}
	}
	inline static thread_local uint64_t *owned = nullptr;
	uint64_t *word, *previous;
	bool acquired;
};
}
