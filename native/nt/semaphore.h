#pragma once

#include "objects.h"
#include "shared_file.h"

namespace raven {

struct SemaphoreState {
	std::atomic<int32_t> count{0};
	int32_t maximum = 0;
};

// A semaphore without a name lives in the process's table (kernel_waits.h); a named one, or one handed to another process, in memory
// the processes share.
class SemaphoreObject : public KernelObject {
public:
	SemaphoreObject() = default;
	// A semaphore another process handed over: its state is the memory the descriptor names.
	explicit SemaphoreObject(int descriptor);
	~SemaphoreObject() override;
	std::u16string_view type_name() const override { return u"Semaphore"; }
	int signaled(uint32_t) const override;
	bool try_consume(uint32_t thread) override;
	void consume(uint32_t thread) override { try_consume(thread); }
	std::atomic<uint32_t> *wait_word() const override
	{
		return state != &local ? reinterpret_cast<std::atomic<uint32_t> *>(&state->count) : nullptr;
	}
	// Adds `amount` to the count, false when it would pass the maximum; `previous` is the count before.
	bool release(int32_t amount, int32_t &previous);
	bool describe(InheritedHandle &record) const override;
	SemaphoreState local;
	mutable SemaphoreState *state = &local;
	std::shared_ptr<SharedFile> shared;
	mutable uint32_t waits_object = 0;

private:
	mutable int descriptor = -1;
};

}
