// The runtime's eventfds are local CLOEXEC wake descriptors: none is duplicated, inherited or sent to another process. A FIFO holds
// just one readiness token; the counter lives here, so many wakes neither fill the pipe nor turn into separate counter reads.
// Interposing these three POSIX entry points keeps Linux callers (including ordinary read/write, not just eventfd_read/write) intact.
// Non-event descriptors go straight to libSystem. Only POLLIN readiness is used by Raven; FIFO POLLOUT is not counter-overflow readiness.
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>
#include <poll.h>

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace {

template<typename Function> Function system_function(const char *name)
{
	return reinterpret_cast<Function>(dlsym(RTLD_NEXT, name));
}

ssize_t system_read(int descriptor, void *buffer, size_t size)
{
	static const auto function = system_function<ssize_t (*)(int, void *, size_t)>("read");
	return function(descriptor, buffer, size);
}

ssize_t system_write(int descriptor, const void *buffer, size_t size)
{
	static const auto function = system_function<ssize_t (*)(int, const void *, size_t)>("write");
	return function(descriptor, buffer, size);
}

int system_close(int descriptor)
{
	static const auto function = system_function<int (*)(int)>("close");
	return function(descriptor);
}

struct Counter {
	std::mutex mutex;
	std::condition_variable changed;
	uint64_t value;
	bool semaphore;
	bool closed = false;
	Counter(uint64_t value, bool semaphore) : value(value), semaphore(semaphore) {}
};

struct Events {
	std::mutex mutex;
	std::map<int, std::shared_ptr<Counter>> descriptors;
	std::atomic<size_t> count{0};
};

Events &events()
{
	// The shared-change listener is detached and can still write during static destruction.
	static auto *table = new Events;
	return *table;
}

std::shared_ptr<Counter> counter(int descriptor)
{
	auto &table = events();
	if (!table.count.load(std::memory_order_relaxed))
		return nullptr;
	std::lock_guard lock(table.mutex);
	const auto found = table.descriptors.find(descriptor);
	return found == table.descriptors.end() ? nullptr : found->second;
}

ssize_t failure(int error)
{
	errno = error;
	return -1;
}

}

extern "C" int eventfd(unsigned int initial, int flags)
{
	if (flags & ~(EFD_NONBLOCK | EFD_CLOEXEC | EFD_SEMAPHORE))
		return int(failure(EINVAL));
	char directory[] = "/tmp/raven-eventfd-XXXXXX";
	if (!mkdtemp(directory))
		return -1;
	const std::string path = std::string(directory) + "/counter";
	if (mkfifo(path.c_str(), 0600) < 0) {
		const int error = errno;
		rmdir(directory);
		return int(failure(error));
	}
	const int descriptor = open(path.c_str(), O_RDWR | (flags & (O_CLOEXEC | O_NONBLOCK)));
	const int error = errno;
	unlink(path.c_str());
	rmdir(directory);
	if (descriptor < 0)
		return int(failure(error));
	try {
		auto state = std::make_shared<Counter>(initial, flags & EFD_SEMAPHORE);
		if (initial) {
			const char token = 1;
			if (system_write(descriptor, &token, 1) != 1) {
				const int error = errno;
				system_close(descriptor);
				return int(failure(error));
			}
		}
		auto &table = events();
		std::lock_guard lock(table.mutex);
		table.descriptors.emplace(descriptor, std::move(state));
		table.count.fetch_add(1, std::memory_order_relaxed);
		return descriptor;
	} catch (...) {
		system_close(descriptor);
		return int(failure(ENOMEM));
	}
}

extern "C" ssize_t read(int descriptor, void *buffer, size_t size)
{
	const auto state = counter(descriptor);
	if (!state)
		return system_read(descriptor, buffer, size);
	if (size < sizeof(uint64_t))
		return failure(EINVAL);
	if (!buffer)
		return failure(EFAULT);
	std::unique_lock lock(state->mutex);
	while (!state->value) {
		if (state->closed)
			return failure(EBADF);
		if (fcntl(descriptor, F_GETFL) & O_NONBLOCK)
			return failure(EAGAIN);
		lock.unlock();
		pollfd wait{descriptor, POLLIN, 0};
		const int ready = poll(&wait, 1, -1);
		if (ready < 0)
			return -1;
		lock.lock();
	}
	const uint64_t value = state->semaphore ? 1 : state->value;
	if (state->value == value) {
		char token;
		if (system_read(descriptor, &token, 1) != 1)
			return -1;
	}
	state->value -= value;
	std::memcpy(buffer, &value, sizeof value);
	state->changed.notify_all();
	return sizeof value;
}

extern "C" ssize_t write(int descriptor, const void *buffer, size_t size)
{
	const auto state = counter(descriptor);
	if (!state)
		return system_write(descriptor, buffer, size);
	if (size != sizeof(uint64_t))
		return failure(EINVAL);
	if (!buffer)
		return failure(EFAULT);
	uint64_t value;
	std::memcpy(&value, buffer, sizeof value);
	if (value == UINT64_MAX)
		return failure(EINVAL);
	std::unique_lock lock(state->mutex);
	while (value > UINT64_MAX - 1 - state->value) {
		if (fcntl(descriptor, F_GETFL) & O_NONBLOCK)
			return failure(EAGAIN);
		state->changed.wait(lock);
		if (state->closed)
			return failure(EBADF);
	}
	if (state->closed)
		return failure(EBADF);
	if (!state->value && value) {
		const char token = 1;
		if (system_write(descriptor, &token, 1) != 1)
			return -1;
	}
	state->value += value;
	return sizeof value;
}

extern "C" void raven_socket_closing(int descriptor);

extern "C" int close(int descriptor)
{
	raven_socket_closing(descriptor);
	auto &table = events();
	if (table.count.load(std::memory_order_relaxed)) {
		std::lock_guard lock(table.mutex);
		const auto found = table.descriptors.find(descriptor);
		if (found != table.descriptors.end()) {
			const auto state = found->second;
			std::lock_guard state_lock(state->mutex);
			state->closed = true;
			state->changed.notify_all();
			table.descriptors.erase(found);
			table.count.fetch_sub(1, std::memory_order_relaxed);
		}
	}
	return system_close(descriptor);
}

extern "C" int eventfd_read(int descriptor, eventfd_t *value)
{
	return read(descriptor, value, sizeof *value) == sizeof *value ? 0 : -1;
}

extern "C" int eventfd_write(int descriptor, eventfd_t value)
{
	return write(descriptor, &value, sizeof value) == sizeof value ? 0 : -1;
}
