#include "kernel_thunks.h"
#include "nt/api.h"
#include "nt/prefix.h"
#include "nt/file_objects.h"

#include <array>
#include <chrono>
#include <ctime>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include "nt/threads.h"
#include <signal.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#import <Metal/Metal.h>

namespace raven {
namespace {

constexpr uint32_t status_invalid_parameter = 0xc000000dU;
constexpr uint32_t status_invalid_handle = 0xc0000008U;
constexpr uint32_t status_not_supported = 0xc00000bbU;
constexpr uint32_t status_buffer_too_small = 0xc0000023U;
constexpr uint32_t status_timeout = 0x00000102U, status_abandoned = 0x00000080U;
constexpr uint32_t resource_magic = 0x52445854U;
constexpr uint32_t mutex_magic = 0x524b4d54U;

struct ScopedDescriptor {
	int value;
	~ScopedDescriptor() { if (value >= 0) close(value); }
};

struct ScopeExit {
	std::function<void()> finish;
	~ScopeExit() { finish(); }
};

struct MutexFile {
	uint32_t magic, held;
	uint64_t key, fence;
	uint32_t size;
	std::array<uint8_t, 512> runtime;
};

std::filesystem::path mutex_path(uint32_t global)
{
	return NativePrefix::from_environment().root() / "raven-dxmt-mutexes" / std::to_string(global);
}

struct ResourceFile {
	uint32_t magic, owner, size;
	std::array<uint8_t, 512> runtime;
};

std::filesystem::path resource_path(uint32_t global)
{
	return NativePrefix::from_environment().root() / "raven-dxmt-shares" / std::to_string(global);
}

bool read_resource(uint32_t global, ResourceFile &file)
{
	if ((global & 0xc0000000U) != 0xc0000000U) return false;
	std::ifstream input(resource_path(global), std::ios::binary);
	return bool(input.read(reinterpret_cast<char *>(&file), sizeof(file))) && file.magic == resource_magic &&
		file.size && file.size <= file.runtime.size() && file.owner &&
		(!kill(pid_t(file.owner), 0) || errno != ESRCH);
}

bool read_resource_fd(int descriptor, ResourceFile &file)
{
	return pread(descriptor, &file, sizeof(file), 0) == sizeof(file) && file.magic == resource_magic &&
		file.size && file.size <= file.runtime.size() && file.owner &&
		(!kill(pid_t(file.owner), 0) || errno != ESRCH);
}

bool write_resource(uint32_t global, const ResourceFile &file)
{
	std::error_code error;
	std::filesystem::create_directories(resource_path(global).parent_path(), error);
	if (error) return false;
	const int fd = open(resource_path(global).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0) return false;
	const bool written = write(fd, &file, sizeof(file)) == sizeof(file);
	close(fd);
	if (!written) std::filesystem::remove(resource_path(global), error);
	return written;
}

struct Adapters {
	std::mutex mutex;
	uint32_t next = 1;
	std::map<uint32_t, uint64_t> adapters;
	std::map<uint32_t, uint32_t> devices;
	struct Resource { uint32_t device, global; bool creator; };
	std::map<uint32_t, Resource> resources;
	struct Mutex { int descriptor; uint32_t global; bool creator, held = false, acquiring = false; };
	std::map<uint32_t, Mutex> mutexes;
	~Adapters() {
        for (const auto &[handle, mutex] : mutexes) {
            if (mutex.creator) unlink(mutex_path(mutex.global).c_str());
            close(mutex.descriptor);
        }
    }

	uint32_t handle() { return 0xc0000000U | next++; }
};

// DXGI derives its LUID from the byte-swapped Metal registry ID. An adapter handle
// is valid only for a device actually present on this Mac.
bool metal_luid(uint64_t luid)
{
	for (id<MTLDevice> device in MTLCopyAllDevices())
		if (__builtin_bswap64(device.registryID) == luid)
			return true;
	return false;
}

}

void register_darwin_d3dkmt(ApiRegistry &registry, NativeObjects &objects, NativeThreads &threads, bool keyed_only)
{
	auto state = std::make_shared<Adapters>();
	if (!keyed_only) {
	auto nt_resource = [&objects](uint64_t handle, ResourceFile &file) {
		const auto object = objects.object_as<FileObject>(handle);
		return object && object->path.parent_path() == resource_path(0).parent_path() &&
			read_resource_fd(object->descriptor, file);
	};
	registry.add("D3DKMTOpenAdapterFromLuid", 1, [state](ApiCall &call) {
		if (!call[0]) return call.ret(status_invalid_parameter);
		const uint64_t luid = call.load<uint64_t>(call[0]);
		if (!metal_luid(luid)) return call.ret(status_not_supported);
		std::lock_guard lock(state->mutex);
		const uint32_t handle = state->handle();
		state->adapters.emplace(handle, luid);
		call.store<uint32_t>(call[0] + 8, handle);
		call.ret(0);
	});
	registry.add("D3DKMTCloseAdapter", 1, [state](ApiCall &call) {
		if (!call[0]) return call.ret(status_invalid_parameter);
		std::lock_guard lock(state->mutex);
		call.ret(state->adapters.erase(call.load<uint32_t>(call[0])) ? 0 : status_invalid_handle);
	});
	registry.add("D3DKMTCreateDevice", 1, [state](ApiCall &call) {
		if (!call[0]) return call.ret(status_invalid_parameter);
		const uint32_t adapter = call.load<uint32_t>(call[0]);
		std::lock_guard lock(state->mutex);
		if (!state->adapters.contains(adapter)) return call.ret(status_invalid_handle);
		const uint32_t handle = state->handle();
		state->devices.emplace(handle, adapter);
		// hAdapter is an eight-byte union with pAdapter on x64; Flags follows it.
		call.store<uint32_t>(call[0] + 12, handle);
		call.ret(0);
	});
	registry.add("D3DKMTDestroyDevice", 1, [state](ApiCall &call) {
		if (!call[0]) return call.ret(status_invalid_parameter);
		std::lock_guard lock(state->mutex);
		call.ret(state->devices.erase(call.load<uint32_t>(call[0])) ? 0 : status_invalid_handle);
	});
	registry.add("D3DKMTCreateAllocation2", 1, [state](ApiCall &call) {
		if (!call[0]) return call.ret(status_invalid_parameter);
		const uint32_t device = call.load<uint32_t>(call[0]);
		const uint64_t runtime = call.load<uint64_t>(call[0] + 16);
		const uint32_t size = call.load<uint32_t>(call[0] + 24);
		const uint32_t allocations = call.load<uint32_t>(call[0] + 44);
		const uint64_t allocation_info = call.load<uint64_t>(call[0] + 48);
		const uint32_t flags = call.load<uint32_t>(call[0] + 56);
		if (!runtime || !size || size > 512 || allocations != 1 || !allocation_info ||
			(flags & 3) != 3) return call.ret(status_invalid_parameter);
		std::lock_guard lock(state->mutex);
		if (!state->devices.contains(device)) return call.ret(status_invalid_handle);
		ResourceFile file{};
		file.magic = resource_magic;
		file.owner = uint32_t(getpid());
		file.size = size;
		const auto bytes = call.read(runtime, size);
		std::copy(bytes.begin(), bytes.end(), file.runtime.begin());
		uint32_t global = 0;
		for (unsigned try_count = 0; try_count < 32; ++try_count) {
			global = 0xc0000000U | arc4random_uniform(0x40000000U);
			if (write_resource(global, file)) break;
			global = 0;
		}
		if (!global) return call.ret(status_not_supported);
		const uint32_t local = state->handle();
		state->resources.emplace(local, Adapters::Resource{device, global, true});
		call.store<uint32_t>(call[0] + 4, local);
		call.store<uint32_t>(call[0] + 8, global);
		call.store<uint32_t>(allocation_info, state->handle());
		call.ret(0);
	});
	registry.add("D3DKMTQueryResourceInfo", 1, [state](ApiCall &call) {
		if (!call[0]) return call.ret(status_invalid_parameter);
		const uint32_t device = call.load<uint32_t>(call[0]);
		const uint32_t global = call.load<uint32_t>(call[0] + 4);
		std::lock_guard lock(state->mutex);
		if (!state->devices.contains(device)) return call.ret(status_invalid_handle);
		ResourceFile file{};
		if (!read_resource(global, file)) return call.ret(status_invalid_handle);
		const uint64_t output = call.load<uint64_t>(call[0] + 8);
		const uint32_t capacity = call.load<uint32_t>(call[0] + 16);
		call.store<uint32_t>(call[0] + 16, file.size);
		call.store<uint32_t>(call[0] + 20, 0);
		call.store<uint32_t>(call[0] + 24, 0);
		call.store<uint32_t>(call[0] + 28, 1);
		if (!output || capacity < file.size) return call.ret(status_buffer_too_small);
		call.write(output, std::span(file.runtime.data(), file.size));
		call.ret(0);
	});
	registry.add("D3DKMTOpenResource2", 1, [state](ApiCall &call) {
		if (!call[0]) return call.ret(status_invalid_parameter);
		const uint32_t device = call.load<uint32_t>(call[0]);
		const uint32_t global = call.load<uint32_t>(call[0] + 4);
		const uint32_t allocations = call.load<uint32_t>(call[0] + 8);
		const uint64_t info = call.load<uint64_t>(call[0] + 16);
		const uint64_t output = call.load<uint64_t>(call[0] + 24);
		const uint32_t capacity = call.load<uint32_t>(call[0] + 32);
		if (allocations != 1 || !info || !output) return call.ret(status_invalid_parameter);
		std::lock_guard lock(state->mutex);
		if (!state->devices.contains(device)) return call.ret(status_invalid_handle);
		ResourceFile file{};
		if (!read_resource(global, file)) return call.ret(status_invalid_handle);
		call.store<uint32_t>(call[0] + 32, file.size);
		if (capacity < file.size) return call.ret(status_buffer_too_small);
		call.write(output, std::span(file.runtime.data(), file.size));
		const uint32_t local = state->handle();
		state->resources.emplace(local, Adapters::Resource{device, global, false});
		call.store<uint32_t>(info, state->handle());
		call.store<uint32_t>(call[0] + 68, local);
		call.ret(0);
	});
	registry.add("D3DKMTDestroyAllocation", 1, [state](ApiCall &call) {
		if (!call[0]) return call.ret(status_invalid_parameter);
		const uint32_t device = call.load<uint32_t>(call[0]);
		const uint32_t local = call.load<uint32_t>(call[0] + 4);
		std::lock_guard lock(state->mutex);
		const auto resource = state->resources.find(local);
		if (resource == state->resources.end() || resource->second.device != device)
			return call.ret(status_invalid_handle);
		if (resource->second.creator) {
			std::error_code error;
			std::filesystem::remove(resource_path(resource->second.global), error);
		}
		state->resources.erase(resource);
		call.ret(0);
	});
	registry.add("D3DKMTShareObjects", 5, [state, &objects](ApiCall &call) {
		if (call.u32(0) != 1 || !call[1] || !call[4]) return call.ret(status_invalid_parameter);
		const uint32_t local = call.load<uint32_t>(call[1]);
		std::lock_guard lock(state->mutex);
		const auto resource = state->resources.find(local);
		if (resource == state->resources.end()) return call.ret(status_invalid_handle);
		const auto path = resource_path(resource->second.global);
		const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC);
		if (descriptor < 0) return call.ret(status_invalid_handle);
		auto object = std::make_shared<FileObject>(descriptor, 0x80000000U, false, path, u"", 0);
		const auto made = objects.add_object(object);
		if (made.status) return call.ret(made.status);
		if (call[2]) {
			const uint64_t attributes = call.load<uint64_t>(call[2] + 32);
			if (attributes && call.load<uint32_t>(attributes + 16)) objects.set_inherit(made.value, true);
		}
		call.store_pointer(call[4], made.value);
		call.ret(0);
	});
	registry.add("D3DKMTQueryResourceInfoFromNtHandle", 1, [state, nt_resource](ApiCall &call) {
		if (!call[0]) return call.ret(status_invalid_parameter);
		const uint32_t device = call.load<uint32_t>(call[0]);
		const uint64_t handle = call.load<uint64_t>(call[0] + 8);
		std::lock_guard lock(state->mutex);
		if (!state->devices.contains(device)) return call.ret(status_invalid_handle);
		ResourceFile file{};
		if (!nt_resource(handle, file)) return call.ret(status_invalid_handle);
		const uint64_t output = call.load<uint64_t>(call[0] + 16);
		const uint32_t capacity = call.load<uint32_t>(call[0] + 24);
		call.store<uint32_t>(call[0] + 24, file.size);
		call.store<uint32_t>(call[0] + 28, 0);
		call.store<uint32_t>(call[0] + 32, 0);
		call.store<uint32_t>(call[0] + 36, 1);
		if (!output || capacity < file.size) return call.ret(status_buffer_too_small);
		call.write(output, std::span(file.runtime.data(), file.size));
		call.ret(0);
	});
	registry.add("D3DKMTOpenResourceFromNtHandle", 1, [state, nt_resource](ApiCall &call) {
		if (!call[0]) return call.ret(status_invalid_parameter);
		const uint32_t device = call.load<uint32_t>(call[0]);
		const uint64_t handle = call.load<uint64_t>(call[0] + 8);
		const uint32_t allocations = call.load<uint32_t>(call[0] + 16);
		const uint64_t info = call.load<uint64_t>(call[0] + 24);
		const uint32_t capacity = call.load<uint32_t>(call[0] + 32);
		const uint64_t output = call.load<uint64_t>(call[0] + 40);
		if (allocations != 1 || !info || !output) return call.ret(status_invalid_parameter);
		std::lock_guard lock(state->mutex);
		if (!state->devices.contains(device)) return call.ret(status_invalid_handle);
		ResourceFile file{};
		if (!nt_resource(handle, file)) return call.ret(status_invalid_handle);
		call.store<uint32_t>(call[0] + 32, file.size);
		if (capacity < file.size) return call.ret(status_buffer_too_small);
		call.write(output, std::span(file.runtime.data(), file.size));
		const uint32_t local = state->handle();
		state->resources.emplace(local, Adapters::Resource{device, 0, false});
		call.store<uint32_t>(info, state->handle());
		call.store<uint32_t>(call[0] + 80, local);
		call.ret(0);
	});
	}
	registry.add("D3DKMTCreateKeyedMutex2", 1, [state](ApiCall &call) {
		if (!call[0]) return call.ret(status_invalid_parameter);
		const bool x86 = call.pointer_width() == 4;
		const uint64_t data = call.load_pointer(call[0] + 16);
		const uint32_t size = call.load<uint32_t>(call[0] + (x86 ? 20 : 24));
		if ((size && !data) || size > 512 || call.load<uint32_t>(call[0] + (x86 ? 24 : 28)) & ~1U)
			return call.ret(status_invalid_parameter);
		if (call.load<uint32_t>(call[0] + (x86 ? 24 : 28)) & 1U) return call.ret(status_not_supported);
		MutexFile file{};
		file.magic = mutex_magic;
		file.key = call.load<uint64_t>(call[0]);
		file.size = size;
		const auto name = size ? call.read(data, size) : std::vector<uint8_t>{};
		std::copy(name.begin(), name.end(), file.runtime.begin());
		std::error_code error;
		std::filesystem::create_directories(mutex_path(0).parent_path(), error);
		if (error) return call.ret(status_not_supported);
		uint32_t global = 0;
		int descriptor = -1;
		for (unsigned attempt = 0; attempt < 32; ++attempt) {
			global = 0xc0000000U | arc4random_uniform(0x40000000U);
			descriptor = open(mutex_path(global).c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
			if (descriptor >= 0) break;
		}
		if (descriptor < 0) return call.ret(status_not_supported);
		if (pwrite(descriptor, &file, sizeof file, 0) != sizeof file) {
			close(descriptor);
			std::filesystem::remove(mutex_path(global), error);
			return call.ret(status_not_supported);
		}
		std::lock_guard lock(state->mutex);
		const uint32_t local = state->handle();
		state->mutexes.emplace(local, Adapters::Mutex{descriptor, global, true});
		call.store<uint32_t>(call[0] + 8, global);
		call.store<uint32_t>(call[0] + 12, local);
		call.ret(0);
	});
	registry.add("D3DKMTOpenKeyedMutex2", 1, [state](ApiCall &call) {
		if (!call[0]) return call.ret(status_invalid_parameter);
		const uint32_t global = call.load<uint32_t>(call[0]);
		const bool x86 = call.pointer_width() == 4;
		const uint64_t output = call.load_pointer(call[0] + 8);
		const uint32_t capacity = call.load<uint32_t>(call[0] + (x86 ? 12 : 16));
		if (!output && capacity) return call.ret(status_invalid_parameter);
		const int descriptor = open(mutex_path(global).c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
		if (descriptor < 0) return call.ret(status_invalid_handle);
		MutexFile file{};
		if (pread(descriptor, &file, sizeof file, 0) != sizeof file || file.magic != mutex_magic || file.size > file.runtime.size()) {
			close(descriptor);
			return call.ret(status_invalid_handle);
		}
		call.store<uint32_t>(call[0] + (x86 ? 12 : 16), file.size);
		if (capacity < file.size) {
			close(descriptor);
			return call.ret(status_buffer_too_small);
		}
		if (file.size) call.write(output, std::span(file.runtime.data(), file.size));
		std::lock_guard lock(state->mutex);
		const uint32_t local = state->handle();
		state->mutexes.emplace(local, Adapters::Mutex{descriptor, global, false});
		call.store<uint32_t>(call[0] + 4, local);
		call.ret(0);
	});
	registry.add("D3DKMTAcquireKeyedMutex", 1, [state, &threads](ApiCall &call) {
		if (!call[0]) return call.ret(status_invalid_parameter);
		const uint32_t local = call.load<uint32_t>(call[0]);
		const uint64_t key = call.load<uint64_t>(call[0] + 8);
		const uint64_t timeout_ptr = call.load_pointer(call[0] + 16);
		using Clock = std::chrono::steady_clock;
		Clock::time_point deadline = Clock::time_point::max();
		if (timeout_ptr) {
			const int64_t timeout = call.load<int64_t>(timeout_ptr);
			uint64_t remaining = timeout < 0 ? uint64_t(-(timeout + 1)) + 1 : 0;
			if (timeout > 0) {
				const auto now = std::chrono::system_clock::now().time_since_epoch();
				const uint64_t windows_ticks = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() / 100) +
					116444736000000000ULL;
				remaining = uint64_t(timeout) > windows_ticks ? uint64_t(timeout) - windows_ticks : 0;
			}
			const auto start = Clock::now();
			const auto limit = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::time_point::max() - start).count();
			deadline = remaining > uint64_t(limit / 100) ? Clock::time_point::max() :
				start + std::chrono::nanoseconds(int64_t(remaining * 100));
		}
		int descriptor = -1;
		{
			std::lock_guard lock(state->mutex);
			auto found = state->mutexes.find(local);
			if (found == state->mutexes.end() || found->second.held || found->second.acquiring)
				return call.ret(status_invalid_handle);
			descriptor = dup(found->second.descriptor);
			if (descriptor < 0) return call.ret(status_not_supported);
			found->second.acquiring = true;
		}
		ScopedDescriptor acquired_descriptor{descriptor};
        ScopeExit clear_acquiring{[state, local] {
            std::lock_guard lock(state->mutex);
            if (auto found = state->mutexes.find(local); found != state->mutexes.end()) found->second.acquiring = false;
        }};
		uint32_t result = status_timeout;
		for (;;) {
			if (flock(descriptor, LOCK_EX | LOCK_NB) == 0) {
				MutexFile file{};
				if (pread(descriptor, &file, sizeof file, 0) != sizeof file || file.magic != mutex_magic || file.size > file.runtime.size()) {
					result = status_invalid_handle;
					flock(descriptor, LOCK_UN);
					break;
				}
				const bool abandoned = file.held != 0;
                if (abandoned) {
                    // An abandoned DXGI resource cannot be reacquired; callers must recreate it.
                    file.held = 2;
                    result = pwrite(descriptor, &file, sizeof file, 0) == sizeof file ? status_abandoned : status_not_supported;
                    flock(descriptor, LOCK_UN);
                    break;
                }
				if (file.key == key) {
					std::lock_guard lock(state->mutex);
					auto found = state->mutexes.find(local);
					if (found == state->mutexes.end()) {
						result = status_invalid_handle;
						flock(descriptor, LOCK_UN);
						break;
					}
					file.held = 1;
					if (pwrite(descriptor, &file, sizeof file, 0) != sizeof file) {
						result = status_not_supported;
						flock(descriptor, LOCK_UN);
						break;
					}
					found->second.held = true;
					call.store<uint64_t>(call[0] + 24, file.fence);
					result = 0;
					break;
				}
				flock(descriptor, LOCK_UN);
			} else if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
				result = status_not_supported;
				break;
			}
			if (Clock::now() >= deadline) break;
			threads.block([] { return false; }, std::min(deadline, Clock::now() + std::chrono::milliseconds(1)));
		}
		call.ret(result);
	});
	registry.add("D3DKMTReleaseKeyedMutex", 1, [state](ApiCall &call) {
		if (!call[0]) return call.ret(status_invalid_parameter);
		const uint32_t local = call.load<uint32_t>(call[0]);
		std::lock_guard lock(state->mutex);
		auto found = state->mutexes.find(local);
		if (found == state->mutexes.end() || !found->second.held) return call.ret(status_invalid_handle);
		MutexFile file{};
		const int descriptor = found->second.descriptor;
		if (pread(descriptor, &file, sizeof file, 0) != sizeof file || file.magic != mutex_magic || file.size > file.runtime.size())
			return call.ret(status_invalid_handle);
		file.key = call.load<uint64_t>(call[0] + 8);
		file.fence = call.load<uint64_t>(call[0] + 16);
		file.held = 0;
		if (pwrite(descriptor, &file, sizeof file, 0) != sizeof file) return call.ret(status_not_supported);
		found->second.held = false;
		flock(descriptor, LOCK_UN);
		call.ret(0);
	});
	registry.add("D3DKMTDestroyKeyedMutex", 1, [state](ApiCall &call) {
		if (!call[0]) return call.ret(status_invalid_parameter);
		const uint32_t local = call.load<uint32_t>(call[0]);
		std::lock_guard lock(state->mutex);
		auto found = state->mutexes.find(local);
		if (found == state->mutexes.end()) return call.ret(status_invalid_handle);
		if (found->second.creator) unlink(mutex_path(found->second.global).c_str());
		close(found->second.descriptor);
		state->mutexes.erase(found);
		call.ret(0);
	});
}
}
