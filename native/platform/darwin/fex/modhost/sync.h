// SPDX-License-Identifier: MIT
#pragma once
#include <condition_variable>
#include <shared_mutex>
#include <chrono>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <cstring>

namespace RavenSync {
std::mutex objects_mutex;
std::unordered_map<void *, std::unique_ptr<std::shared_mutex>> rwlocks;
std::unordered_map<void *, std::unique_ptr<std::condition_variable_any>> conditions;
std::recursive_mutex peb_mutex;
std::shared_mutex &rw(void *p) {
	std::lock_guard<std::mutex> guard(objects_mutex);
	auto &v = rwlocks[p];
	if (!v)
		v = std::make_unique<std::shared_mutex>();
	return *v;
}
std::condition_variable_any &cv(void *p) {
	std::lock_guard<std::mutex> guard(objects_mutex);
	auto &v = conditions[p];
	if (!v)
		v = std::make_unique<std::condition_variable_any>();
	return *v;
}
void init_rw(void *p) {
	std::lock_guard<std::mutex> guard(objects_mutex);
	rwlocks.erase(p);
	*static_cast<uint64_t *>(p) = 0;
}
void init_cv(void *p) {
	std::lock_guard<std::mutex> guard(objects_mutex);
	conditions.erase(p);
	*static_cast<uint64_t *>(p) = 0;
}
void acquire_ex(void *p) {
	rw(p).lock();
}
void acquire_sh(void *p) {
	rw(p).lock_shared();
}
void release_ex(void *p) {
	rw(p).unlock();
}
void release_sh(void *p) {
	rw(p).unlock_shared();
}
uint8_t try_ex(void *p) {
	return rw(p).try_lock();
}
uint8_t try_sh(void *p) {
	return rw(p).try_lock_shared();
}
void acquire_peb() {
	peb_mutex.lock();
}
void release_peb() {
	peb_mutex.unlock();
}
void wake_cv(void *p) {
	cv(p).notify_one();
}
void wake_all_cv(void *p) {
	cv(p).notify_all();
}
using Clock = std::chrono::system_clock;
Clock::time_point deadline(const int64_t *time) {
	if (!time)
		return Clock::time_point::max();
	// Positive NT time is absolute since 1601; negative is relative in 100 ns.
	if (*time < 0) {
		auto now = Clock::now();
		auto ticks = uint64_t(-(*time + 1)) + 1;
		// Darwin system_clock uses microseconds; round up sub-microsecond waits.
		auto duration = std::chrono::duration_cast<Clock::duration>(
			std::chrono::microseconds(ticks / 10 + bool(ticks % 10)));
		if (duration > Clock::time_point::max() - now)
			return Clock::time_point::max();
		return now + duration;
	}
	constexpr int64_t epoch = 116444736000000000ll;
	if (*time < epoch)
		return Clock::time_point::min();
	if (*time - epoch > INT64_MAX / 100)
		return Clock::time_point::max();
	return Clock::time_point(std::chrono::duration_cast<Clock::duration>(
		std::chrono::nanoseconds((*time - epoch) * 100)));
}
struct AdoptedLock {
	std::shared_mutex &mutex;
	bool shared;
	void lock() {
		if (shared)
			mutex.lock_shared();
		else
			mutex.lock();
	}
	void unlock() {
		if (shared)
			mutex.unlock_shared();
		else
			mutex.unlock();
	}
};
int32_t sleep_cv(void *condition, void *lock, const int64_t *timeout, uint32_t flags) {
	if (flags & ~1u)
		return int32_t(0xc000000d);
	AdoptedLock held{rw(lock), bool(flags & 1)};
	return cv(condition).wait_until(held, deadline(timeout)) == std::cv_status::timeout ? 0x102 : 0;
}
struct AddressWait {
	std::mutex mutex;
	std::condition_variable condition;
};
// One condition per address, so waking one address cannot consume another wake.
std::unordered_map<void *, std::unique_ptr<AddressWait>> addresses;
AddressWait &address(void *p) {
	std::lock_guard<std::mutex> guard(objects_mutex);
	auto &v = addresses[p];
	if (!v)
		v = std::make_unique<AddressWait>();
	return *v;
}
int32_t wait_address(void *p, const void *comparison, size_t size, const int64_t *timeout) {
	if (!p || !comparison || (size != 1 && size != 2 && size != 4 && size != 8))
		return int32_t(0xc000000d);
	auto &v = address(p);
	std::unique_lock<std::mutex> lock(v.mutex);
	if (memcmp(p, comparison, size))
		return 0;
	return v.condition.wait_until(lock, deadline(timeout)) == std::cv_status::timeout ? 0x102 : 0;
}
void wake_address(void *p) {
	auto &v = address(p);
	std::lock_guard<std::mutex> guard(v.mutex);
	v.condition.notify_one();
}
void wake_all_address(void *p) {
	auto &v = address(p);
	std::lock_guard<std::mutex> guard(v.mutex);
	v.condition.notify_all();
}
void *resolve(const char *name) {
#define SYNC(n, f) \
	if (!strcmp(name, n)) \
	return reinterpret_cast<void *>(&f)
	SYNC("RtlInitializeSRWLock", init_rw);
	SYNC("RtlAcquireSRWLockExclusive", acquire_ex);
	SYNC("RtlAcquireSRWLockShared", acquire_sh);
	SYNC("RtlReleaseSRWLockExclusive", release_ex);
	SYNC("RtlReleaseSRWLockShared", release_sh);
	SYNC("RtlTryAcquireSRWLockExclusive", try_ex);
	SYNC("RtlTryAcquireSRWLockShared", try_sh);
	SYNC("RtlAcquirePebLock", acquire_peb);
	SYNC("RtlReleasePebLock", release_peb);
	SYNC("RtlInitializeConditionVariable", init_cv);
	SYNC("RtlWakeConditionVariable", wake_cv);
	SYNC("RtlWakeAllConditionVariable", wake_all_cv);
	SYNC("RtlSleepConditionVariableSRW", sleep_cv);
	SYNC("RtlWaitOnAddress", wait_address);
	SYNC("RtlWakeAddressSingle", wake_address);
	SYNC("RtlWakeAddressAll", wake_all_address);
#undef SYNC
	return nullptr;
}
} // namespace RavenSync
