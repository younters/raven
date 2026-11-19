#include "semaphore.h"
#include "kernel_waits.h"

#include <new>
#include <stdexcept>
#include <sys/stat.h>

namespace raven {

SemaphoreObject::SemaphoreObject(int descriptor) : descriptor(descriptor)
{
	try {
		struct stat information{};
		if (descriptor < 0 || ::fstat(descriptor, &information) || information.st_size != sizeof(SemaphoreState))
			throw std::runtime_error("invalid semaphore storage");
		void *mapped = ::mmap(nullptr, sizeof(SemaphoreState), PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
		if (mapped == MAP_FAILED)
			throw std::runtime_error("cannot map semaphore storage");
		state = static_cast<SemaphoreState *>(mapped);
		listen_shared_changes();
		++shared_object_count;
	} catch (...) {
		if (state != &local) ::munmap(state, sizeof(SemaphoreState));
		if (descriptor >= 0) ::close(descriptor);
		throw;
	}
}

SemaphoreObject::~SemaphoreObject()
{
	if (waits_object)
		raven_waits_close(kernel_waits(), waits_object);
	if (descriptor >= 0) {
		::munmap(state, sizeof(SemaphoreState));
		::close(descriptor);
		--shared_object_count;
	}
}

int SemaphoreObject::signaled(uint32_t) const
{
	uint32_t count = 0;
	if (waits_object)
		return raven_waits_query(kernel_waits(), waits_object, &count) == 0 && count > 0;
	return state->count.load() > 0;
}

bool SemaphoreObject::try_consume(uint32_t thread)
{
	if (waits_object)
		return raven_waits_wait(kernel_waits(), thread, &waits_object, 1, 0, 0, 0) == 0;
	auto current = state->count.load();
	while (current > 0)
		if (state->count.compare_exchange_weak(current, current - 1))
			return true;
	return false;
}

bool SemaphoreObject::release(int32_t amount, int32_t &previous)
{
	if (waits_object) {
		uint32_t before = 0;
		if (amount <= 0 || raven_waits_release_semaphore(kernel_waits(), waits_object, uint32_t(amount), &before))
			return false;
		previous = int32_t(before);
		return true;
	}
	auto current = state->count.load();
	for (;;) {
		if (amount <= 0 || int64_t(current) + amount > state->maximum)
			return false;
		if (state->count.compare_exchange_weak(current, current + amount))
			break;
	}
	previous = current;
	if (state != &local) {
		wake_word(wait_word());
		announce_shared_change();
	}
	return true;
}

bool SemaphoreObject::describe(InheritedHandle &record) const
{
	// An unnamed semaphore moves to shared memory once: every handle of this process keeps the same state after a DuplicateHandle into
	// another process, a DUPLICATE_CLOSE_SOURCE transfer among them.
	if (!shared && descriptor < 0) {
		const int created = ::memfd_create("raven-semaphore", MFD_CLOEXEC | MFD_ALLOW_SEALING);
		if (created < 0)
			return false;
		if (::ftruncate(created, sizeof(SemaphoreState)) ||
			::fcntl(created, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL)) {
			::close(created);
			return false;
		}
		void *mapped = ::mmap(nullptr, sizeof(SemaphoreState), PROT_READ | PROT_WRITE, MAP_SHARED, created, 0);
		if (mapped == MAP_FAILED) {
			::close(created);
			return false;
		}
		// The semaphore leaves the table with its count; the waits there for it end, to wait again on the shared one.
		int32_t count = state->count.load();
		if (waits_object) {
			uint32_t left = 0;
			raven_waits_detach(kernel_waits(), std::exchange(waits_object, 0), &left, nullptr, nullptr);
			count = int32_t(left);
		}
		auto *storage = new (mapped) SemaphoreState();
		storage->count = count;
		storage->maximum = state->maximum;
		state = storage;
		descriptor = created;
		listen_shared_changes();
		++shared_object_count;
	}
	record.kind = "semaphore";
	record.descriptor = shared ? shared->file_descriptor() : descriptor;
	return record.descriptor >= 0;
}

}
