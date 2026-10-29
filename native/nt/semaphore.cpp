#include "semaphore.h"

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
	if (descriptor >= 0) {
		::munmap(state, sizeof(SemaphoreState));
		::close(descriptor);
		--shared_object_count;
	}
}

int SemaphoreObject::signaled(uint32_t) const
{
	std::lock_guard lock(mutex);
	return state->count.load() > 0;
}

bool SemaphoreObject::try_consume(uint32_t)
{
	std::lock_guard lock(mutex);
	auto current = state->count.load();
	while (current > 0)
		if (state->count.compare_exchange_weak(current, current - 1))
			return true;
	return false;
}

bool SemaphoreObject::release(int32_t amount, int32_t &previous)
{
	std::lock_guard lock(mutex);
	auto current = state->count.load();
	for (;;) {
		if (amount <= 0 || int64_t(current) + amount > state->maximum) return false;
		if (state->count.compare_exchange_weak(current, current + amount)) break;
	}
	previous = current;
	if (shared || descriptor >= 0) announce_shared_change();
	return true;
}

bool SemaphoreObject::describe(InheritedHandle &record) const
{
	std::lock_guard lock(mutex);
	// Promote an unnamed semaphore once. Every local alias must keep using the
	// same state after a remote DuplicateHandle, including CLOSE_SOURCE transfers.
	if (!shared && descriptor < 0) {
		const int created = ::memfd_create("raven-semaphore", MFD_CLOEXEC | MFD_ALLOW_SEALING);
		if (created < 0) return false;
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
		auto *storage = new (mapped) SemaphoreState();
		storage->count = state->count.load();
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
