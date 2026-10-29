#pragma once

#include "objects.h"
#include "shared_file.h"

namespace raven {

struct SemaphoreState {
	std::atomic<int32_t> count{0};
	int32_t maximum = 0;
};

class SemaphoreObject : public KernelObject {
public:
	SemaphoreObject() = default;
	explicit SemaphoreObject(int descriptor);
	~SemaphoreObject() override;
	std::u16string_view type_name() const override { return u"Semaphore"; }
	int signaled(uint32_t) const override;
	bool try_consume(uint32_t) override;
	void consume(uint32_t thread) override { try_consume(thread); }
	bool release(int32_t amount, int32_t &previous);
	bool describe(InheritedHandle &record) const override;
	SemaphoreState local;
	mutable SemaphoreState *state = &local;
	std::shared_ptr<SharedFile> shared;

private:
	mutable std::mutex mutex;
	mutable int descriptor = -1;
};

}
