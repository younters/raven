#pragma once
#include <cstdint>
#include <cstddef>
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
			while (__atomic_exchange_n(word, uint64_t(1), __ATOMIC_ACQUIRE)) {}
			owned=word;
		}
	}
	~CommitBlockLock()
	{
		if (acquired) { owned=previous; __atomic_store_n(word, uint64_t(0), __ATOMIC_RELEASE); }
	}
	CommitBlockLock(const CommitBlockLock &) = delete;
private:
	inline static thread_local uint64_t *owned = nullptr;
	uint64_t *word, *previous;
	bool acquired;
};
}
