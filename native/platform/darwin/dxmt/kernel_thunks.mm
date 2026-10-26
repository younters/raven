#include "kernel_thunks.h"
#include "nt/api.h"
#include "nt/prefix.h"
#include "nt/file_objects.h"

#include <array>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#import <Metal/Metal.h>

namespace raven {
namespace {

constexpr uint32_t status_invalid_parameter = 0xc000000dU;
constexpr uint32_t status_invalid_handle = 0xc0000008U;
constexpr uint32_t status_not_supported = 0xc00000bbU;
constexpr uint32_t status_buffer_too_small = 0xc0000023U;
constexpr uint32_t resource_magic = 0x52445854U;

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

void register_darwin_d3dkmt(ApiRegistry &registry, NativeObjects &objects)
{
	auto state = std::make_shared<Adapters>();
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
}
