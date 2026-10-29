#include "nt_file_api.h"
#include "nt/api.h"
#include "nt/file_objects.h"
#include "nt/kernel_context.h"
#include "nt/objects.h"
#include "nt/paths.h"

#include <algorithm>

#include <sys/mman.h>
#include <sys/stat.h>
#include <cstring>

// The NT file natives that ntdll exports and programs reach directly, Chromium's sandbox among them: it patches them in each child and
// resolves them by reading ntdll's export table, so they must be there and must work. Each one is the Win32 call that does the same
// work, given the DOS name the NT name stands for, its status translated back.
namespace raven_darwin {
namespace {

using raven::ApiCall;

class Scratch {
public:
	explicit Scratch(raven::NativeProcess &process) : process(process) {}
	~Scratch() { for (const auto address : blocks) process.unmap_memory(address); }
	Scratch(const Scratch &) = delete;
	Scratch &operator=(const Scratch &) = delete;
	uint64_t block(size_t bytes)
	{
		const auto address = process.map_memory((bytes + 4095) & ~size_t(4095), PROT_READ | PROT_WRITE);
		blocks.push_back(address);
		return address;
	}
	uint64_t text(ApiCall &call, std::u16string_view value)
	{
		const auto at = block((value.size() + 1) * 2);
		std::u16string terminated(value);
		terminated.push_back(u'\0');
		call.write(at, std::span(reinterpret_cast<const uint8_t *>(terminated.data()), terminated.size() * 2));
		return at;
	}

private:
	raven::NativeProcess &process;
	std::vector<uint64_t> blocks;
};

constexpr uint32_t status_success = 0, status_invalid_parameter = 0xc000000dU, status_access_violation = 0xc0000005U,
	status_object_name_invalid = 0xc0000033U, status_object_path_not_found = 0xc000003aU, status_object_path_syntax_bad = 0xc000003bU,
	status_not_a_directory = 0xc0000103U, status_file_is_a_directory = 0xc00000baU, status_invalid_info_class = 0xc0000003U,
	status_info_length_mismatch = 0xc0000004U, status_invalid_handle = 0xc0000008U;

uint32_t status_of(uint32_t error)
{
	switch (error) {
	case 0: return status_success;
	case 1: return 0xc0000010U;
	case 2: return 0xc0000034U;
	case 3: return status_object_path_not_found;
	case 4: return 0xc000011fU;
	case 5: return 0xc0000022U;
	case 6: return status_invalid_handle;
	case 8: return 0xc0000017U;
	case 19: return 0xc00000a2U;
	case 32: return 0xc0000043U;
	case 33: return 0xc0000054U;
	case 80: case 183: return 0xc0000035U;
	case 87: return status_invalid_parameter;
	case 112: return 0xc000007fU;
	case 122: return 0xc0000023U;
	case 123: return status_object_name_invalid;
	case 145: return 0xc0000101U;
	case 161: return status_object_path_syntax_bad;
	case 206: return status_object_name_invalid;
	case 231: return 0xc00000aeU;
	case 267: return status_not_a_directory;
	case 998: return status_access_violation;
	default: return 0xc0000001U;
	}
}

// The DOS name of an NT name: drive and UNC paths under \??\, the named pipe and mailslot devices, and a name relative to an open
// directory. Nothing else in the object namespace is a file the runtime can reach.
std::optional<std::u16string> dos_name(raven::NativeObjects &objects, uint64_t root, std::u16string name)
{
	if (root) {
		const auto directory = objects.object_as<raven::FileObject>(root);
		if (!directory || !directory->directory || name.starts_with(u'\\'))
			return std::nullopt;
		auto base = directory->dos_path;
		if (!base.ends_with(u'\\'))
			base.push_back(u'\\');
		return base + name;
	}
	const auto starts = [&](std::u16string_view prefix) {
		if (name.size() < prefix.size())
			return false;
		for (size_t i = 0; i < prefix.size(); ++i) {
			auto unit = name[i];
			if (unit >= u'a' && unit <= u'z')
				unit = char16_t(unit - u'a' + u'A');
			if (unit != prefix[i])
				return false;
		}
		return true;
	};
	for (const std::u16string_view device : {u"\\DEVICE\\NAMEDPIPE\\", u"\\DEVICE\\MAILSLOT\\"})
		if (starts(device))
			return (device[8] == u'N' ? u"\\\\.\\pipe\\" : u"\\\\.\\mailslot\\") + name.substr(device.size());
	for (const std::u16string_view prefix : {u"\\??\\", u"\\DOSDEVICES\\", u"\\GLOBAL??\\"})
		if (starts(prefix)) {
			auto rest = name.substr(prefix.size());
			if (rest.size() >= 2 && rest[1] == u':')
				return rest;
			if (rest.size() >= 4 && (rest[0] == u'U' || rest[0] == u'u') && (rest[1] == u'N' || rest[1] == u'n') &&
				(rest[2] == u'C' || rest[2] == u'c') && rest[3] == u'\\')
				return u"\\\\" + rest.substr(4);
			return u"\\\\.\\" + rest;
		}
	return std::nullopt;
}

struct Attributes {
	uint64_t root = 0;
	uint32_t flags = 0;
	std::u16string name;
};

// OBJECT_ATTRIBUTES: the length, the root directory, the counted name and the attribute flags.
std::optional<Attributes> object_attributes(ApiCall &call, uint64_t address)
{
	if (!address)
		return std::nullopt;
	const size_t width = call.pointer_width();
	Attributes out;
	out.root = call.load_pointer(address + width);
	const uint64_t counted = call.load_pointer(address + width * 2);
	out.flags = call.load<uint32_t>(address + width * 3);
	if (!counted)
		return out;
	const auto length = call.load<uint16_t>(counted);
	const auto buffer = call.load_pointer(counted + width);
	if (length & 1)
		return std::nullopt;
	if (length) {
		out.name.resize(length / 2);
		const auto bytes = call.read(buffer, length);
		std::memcpy(out.name.data(), bytes.data(), length);
	}
	return out;
}

void put_io_status(ApiCall &call, uint64_t block, uint32_t status, uint64_t information)
{
	if (!block)
		return;
	call.store_pointer(block, status);
	call.store_pointer(block + call.pointer_width(), information);
}

// Runs a registered Win32 handler on the given words; the NT caller's last error is left as it was.
struct Inner {
	uint64_t result;
	uint32_t error;
};
Inner run(const std::pair<uint32_t, raven::ApiHandler> &entry, ApiCall &call, std::vector<uint64_t> words)
{
	words.resize(entry.first, 0);
	const auto saved = call.last_error();
	call.set_last_error(0);
	ApiCall inner(call.process, call.environment, std::move(words));
	entry.second(inner);
	const Inner out{inner.result, call.last_error()};
	call.set_last_error(saved);
	return out;
}

uint64_t filetime(const timespec &time)
{
	return (uint64_t(time.tv_sec) + 11644473600ULL) * 10000000ULL + uint64_t(time.tv_nsec) / 100;
}

}

void register_nt_file_api(raven::ApiRegistry &registry, raven::KernelContext &context)
{
	const auto create = registry.find("CreateFileW"), attributes = registry.find("GetFileAttributesExW"),
		directory = registry.find("CreateDirectoryW"), set_information = registry.find("SetFileInformationByHandle"),
		seek = registry.find("SetFilePointerEx"), open_mapping = registry.find("OpenFileMappingW");
	if (!create || !attributes || !directory || !set_information || !seek || !open_mapping)
		return;
	auto &objects = context.objects;
	auto &paths = context.paths;
	constexpr uint32_t directory_attribute = 0x10;

	// NtCreateFile and NtOpenFile: the NT disposition and options become CreateFileW's, after the kind of what the name names is checked
	// against FILE_DIRECTORY_FILE and FILE_NON_DIRECTORY_FILE.
	const auto open = [=, &objects](ApiCall &call, uint64_t out, uint32_t access, uint64_t object, uint64_t io, uint32_t file_attributes,
		uint32_t share, uint32_t disposition, uint32_t options) {
		try {
			const auto parsed = object_attributes(call, object);
			if (!out || !io || !parsed || disposition > 5) {
				call.ret(parsed || !object ? status_invalid_parameter : status_object_name_invalid);
				return;
			}
			const auto name = dos_name(objects, parsed->root, parsed->name);
			if (!name || name->empty()) {
				call.ret(parsed->name.starts_with(u'\\') || parsed->root ? status_object_path_not_found : status_object_path_syntax_bad);
				return;
			}
			constexpr uint32_t directory_file = 0x1, write_through = 0x2, sequential = 0x4, no_buffering = 0x8, synchronous = 0x30,
				non_directory = 0x40, random = 0x800, delete_on_close = 0x1000, backup = 0x4000, reparse = 0x200000;
			if ((options & directory_file) && (options & non_directory)) {
				call.ret(status_invalid_parameter);
				return;
			}
			Scratch scratch(call.process);
			const auto text = scratch.text(call, *name);
			const bool pipe = name->starts_with(u"\\\\.\\");
			const auto data = scratch.block(36);
			const auto found = pipe ? Inner{0, 2} : run(*attributes, call, {text, 0, data});
			const bool exists = found.result != 0;
			const bool is_directory = exists && (call.load<uint32_t>(data) & directory_attribute);
			if (exists && is_directory && (options & non_directory)) {
				call.ret(status_file_is_a_directory);
				return;
			}
			if (exists && !is_directory && (options & directory_file)) {
				call.ret(status_not_a_directory);
				return;
			}
			// A directory is made, not opened, when FILE_DIRECTORY_FILE asks for a new one.
			if ((options & directory_file) && !exists && (disposition == 2 || disposition == 3)) {
				const auto made = run(*directory, call, {text, 0});
				if (!made.result) {
					call.ret(status_of(made.error));
					return;
				}
			} else if ((options & directory_file) && !exists && !pipe) {
				call.ret(status_of(found.error ? found.error : 2));
				return;
			}
			static constexpr uint32_t dispositions[] = {2, 3, 1, 4, 5, 2};
			uint32_t win32 = dispositions[disposition];
			if (is_directory || (options & directory_file))
				win32 = 3;
			uint32_t flags = file_attributes & 0x7fa7;
			if (!(options & synchronous)) flags |= 0x40000000U;
			if (options & write_through) flags |= 0x80000000U;
			if (options & no_buffering) flags |= 0x20000000U;
			if (options & random) flags |= 0x10000000U;
			if (options & sequential) flags |= 0x08000000U;
			if (options & delete_on_close) flags |= 0x04000000U;
			if (options & reparse) flags |= 0x00200000U;
			if ((options & backup) || is_directory || (options & directory_file)) flags |= 0x02000000U;
			uint64_t security = 0;
			if (parsed->flags & 0x2) {
				const size_t width = call.pointer_width();
				security = scratch.block(width * 3);
				call.store<uint32_t>(security, uint32_t(width * 3));
				call.store<uint32_t>(security + width * 2, 1);
			}
			const auto opened = run(*create, call, {text, access, share, security, win32, flags, 0});
			if (opened.result == UINT64_MAX || uint32_t(opened.result) == 0xffffffffU || !opened.result) {
				const uint32_t status = status_of(opened.error ? opened.error : 2);
				put_io_status(call, io, status, 0);
				call.ret(status);
				return;
			}
			constexpr uint64_t superseded = 0, opened_existing = 1, created = 2, overwritten = 3;
			uint64_t information = opened_existing;
			if (!exists && !(options & directory_file))
				information = created;
			else if ((options & directory_file) && !exists)
				information = created;
			else if (disposition == 0)
				information = superseded;
			else if (disposition == 4 || disposition == 5)
				information = overwritten;
			call.store_pointer(out, opened.result);
			put_io_status(call, io, status_success, information);
			call.ret(status_success);
		} catch (const raven::ApiFault &) {
			call.ret(status_access_violation);
		}
	};
	registry.add("NtCreateFile", 11, [open](ApiCall &call) {
		open(call, call[0], call.u32(1), call[2], call[3], call.u32(5), call.u32(6), call.u32(7), call.u32(8));
	});
	registry.add("NtOpenFile", 6, [open](ApiCall &call) {
		open(call, call[0], call.u32(1), call[2], call[3], 0, call.u32(4), 1, call.u32(5));
	});

	// NtQueryAttributesFile fills FILE_BASIC_INFORMATION and NtQueryFullAttributesFile FILE_NETWORK_OPEN_INFORMATION, by name, without
	// opening the file.
	const auto query = [=, &objects, &paths](ApiCall &call, bool full) {
		try {
			const auto parsed = object_attributes(call, call[0]);
			if (!parsed || !call[1]) {
				call.ret(status_invalid_parameter);
				return;
			}
			const auto name = dos_name(objects, parsed->root, parsed->name);
			if (!name || name->empty() || name->starts_with(u"\\\\.\\")) {
				call.ret(status_object_path_not_found);
				return;
			}
			Scratch scratch(call.process);
			const auto text = scratch.text(call, *name);
			const auto data = scratch.block(36);
			const auto found = run(*attributes, call, {text, 0, data});
			if (!found.result) {
				call.ret(status_of(found.error ? found.error : 2));
				return;
			}
			const auto win32 = call.read(data, 36);
			uint32_t attribute;
			uint64_t creation, access, write, size_high, size_low;
			std::memcpy(&attribute, win32.data(), 4);
			std::memcpy(&creation, win32.data() + 4, 8);
			std::memcpy(&access, win32.data() + 12, 8);
			std::memcpy(&write, win32.data() + 20, 8);
			size_high = 0, size_low = 0;
			std::memcpy(&size_high, win32.data() + 28, 4);
			std::memcpy(&size_low, win32.data() + 32, 4);
			const uint64_t size = (size_high << 32) | size_low;
			uint64_t change = write, allocation = (size + 4095) & ~uint64_t(4095);
			if (const auto resolved = paths.resolve(*name)) {
				struct stat metadata{};
				if (!::stat(resolved->host.c_str(), &metadata)) {
					change = filetime(metadata.st_ctimespec);
					allocation = uint64_t(metadata.st_blocks) * 512;
				}
			}
			std::vector<uint8_t> out(full ? 56 : 40, 0);
			std::memcpy(out.data(), &creation, 8);
			std::memcpy(out.data() + 8, &access, 8);
			std::memcpy(out.data() + 16, &write, 8);
			std::memcpy(out.data() + 24, &change, 8);
			if (full) {
				const uint64_t end = (attribute & directory_attribute) ? 0 : size;
				if (attribute & directory_attribute)
					allocation = 0;
				std::memcpy(out.data() + 32, &allocation, 8);
				std::memcpy(out.data() + 40, &end, 8);
				std::memcpy(out.data() + 48, &attribute, 4);
			} else
				std::memcpy(out.data() + 32, &attribute, 4);
			call.write(call[1], out);
			call.ret(status_success);
		} catch (const raven::ApiFault &) {
			call.ret(status_access_violation);
		}
	};
	registry.add("NtQueryAttributesFile", 2, [query](ApiCall &call) { query(call, false); });
	registry.add("NtQueryFullAttributesFile", 2, [query](ApiCall &call) { query(call, true); });

	// NtOpenSection: a named section of the session's namespace (\Sessions\N\BaseNamedObjects, or a name relative to its directory) is
	// the name CreateFileMapping was given, with or without Local\; one of the global namespace is Global\.
	registry.add("NtOpenSection", 3, [=](ApiCall &call) {
		try {
			const auto parsed = object_attributes(call, call[2]);
			if (!call[0] || !parsed || parsed->name.empty()) {
				call.ret(parsed ? status_object_name_invalid : status_invalid_parameter);
				return;
			}
			std::vector<std::u16string> names;
			const auto &name = parsed->name;
			constexpr std::u16string_view global = u"\\BaseNamedObjects\\", sessions = u"\\Sessions\\";
			if (parsed->root && !name.starts_with(u'\\'))
				names = {name, u"Local\\" + name};
			else if (name.starts_with(global))
				names = {u"Global\\" + name.substr(global.size()), name.substr(global.size())};
			else if (name.starts_with(sessions)) {
				const auto objects_at = name.find(global, sessions.size());
				if (objects_at == std::u16string::npos) {
					call.ret(status_object_path_not_found);
					return;
				}
				const auto rest = name.substr(objects_at + global.size());
				names = {rest, u"Local\\" + rest};
			} else {
				call.ret(status_object_path_not_found);
				return;
			}
			// SECTION_MAP_EXECUTE is FILE_MAP_EXECUTE's bit 0x20 in the Win32 call; the other section rights are the same bits.
			uint32_t access = call.u32(1);
			if (access & 0x8)
				access = (access & ~0x8U) | 0x20;
			Scratch scratch(call.process);
			Inner opened{0, 2};
			for (const auto &candidate : names) {
				opened = run(*open_mapping, call, {access, (parsed->flags & 0x2) ? 1U : 0U, scratch.text(call, candidate)});
				if (opened.result || opened.error != 2)
					break;
			}
			if (!opened.result) {
				call.ret(status_of(opened.error ? opened.error : 2));
				return;
			}
			call.store_pointer(call[0], opened.result);
			call.ret(status_success);
		} catch (const raven::ApiFault &) {
			call.ret(status_access_violation);
		}
	});

	// NtSetInformationFile: the classes whose structures SetFileInformationByHandle takes unchanged, and the position, which is
	// SetFilePointerEx's.
	registry.add("NtSetInformationFile", 5, [=, &objects](ApiCall &call) {
		try {
			if (!call[1] || !call[2]) {
				call.ret(status_access_violation);
				return;
			}
			if (!objects.object_as<raven::FileObject>(call[0])) {
				call.ret(status_invalid_handle);
				return;
			}
			const uint32_t length = call.u32(3), information = call.u32(4);
			const size_t width = call.pointer_width();
			struct Class {
				uint32_t nt, win32, minimum;
			};
			const Class classes[] = {{4, 0, 40}, {10, 3, uint32_t(width * 2 + 4)}, {13, 4, 1}, {19, 5, 8}, {20, 6, 8},
				{64, 21, 4}, {65, 22, uint32_t(width * 2 + 4)}};
			if (information == 14) {
				if (length < 8) {
					call.ret(status_info_length_mismatch);
					return;
				}
				const auto moved = run(*seek, call, {call[0], call.load<uint64_t>(call[2]), 0, 0});
				const uint32_t status = moved.result ? status_success : status_of(moved.error);
				put_io_status(call, call[1], status, 0);
				call.ret(status);
				return;
			}
			const auto found = std::find_if(std::begin(classes), std::end(classes), [&](const Class &item) { return item.nt == information; });
			if (found == std::end(classes)) {
				call.ret(status_invalid_info_class);
				return;
			}
			if (length < found->minimum) {
				call.ret(status_info_length_mismatch);
				return;
			}
			uint64_t buffer = call[2];
			uint32_t size = length;
			Scratch scratch(call.process);
			// A new name given as an NT path or relative to a directory handle becomes the DOS path the Win32 structure holds.
			if (information == 10 || information == 65) {
				const size_t header = width * 2 + 4;
				const auto record = call.read(call[2], header);
				uint64_t root = 0;
				uint32_t bytes = 0;
				std::memcpy(&root, record.data() + width, width);
				std::memcpy(&bytes, record.data() + width * 2, 4);
				if ((bytes & 1) || header + bytes > length) {
					call.ret(status_invalid_parameter);
					return;
				}
				std::u16string target(bytes / 2, u'\0');
				if (bytes)
					std::memcpy(target.data(), call.read(call[2] + header, bytes).data(), bytes);
				std::optional<std::u16string> name = target;
				if (root || target.starts_with(u'\\'))
					name = dos_name(objects, root, target);
				if (!name) {
					call.ret(status_object_path_not_found);
					return;
				}
				std::vector<uint8_t> converted(record.begin(), record.begin() + header);
				std::memset(converted.data() + width, 0, width);
				const uint32_t converted_bytes = uint32_t(name->size() * 2);
				std::memcpy(converted.data() + width * 2, &converted_bytes, 4);
				converted.resize(header + converted_bytes + 2, 0);
				std::memcpy(converted.data() + header, name->data(), converted_bytes);
				buffer = scratch.block(converted.size());
				size = uint32_t(converted.size());
				call.write(buffer, converted);
			}
			const auto done = run(*set_information, call, {call[0], found->win32, buffer, size});
			const uint32_t status = done.result ? status_success : status_of(done.error);
			put_io_status(call, call[1], status, 0);
			call.ret(status);
		} catch (const raven::ApiFault &) {
			call.ret(status_access_violation);
		}
	});
}

}
