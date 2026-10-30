// SPDX-License-Identifier: MIT
#pragma once
#include "nt_system.h"
namespace RavenNT {
bool feature(const char *name) {
	int v = 0;
	size_t size = sizeof(v);
	return !sysctlbyname(name, &v, &size, nullptr, 0) && v;
}
int32_t open_key(void **out, uint32_t, ObjectAttributes *object, uint32_t options) {
	if (out)
		*out = nullptr;
	if (!out || !object || !object->name)
		return invalid_parameter;
	if (options)
		return not_supported;
	auto name = encode(static_cast<char16_t *>(object->name->buffer), object->name->length / 2);
	if (object->root) {
		auto root = get_handle(object->root);
		if (!root || root->kind != Handle::key)
			return bad_handle;
		name = root->path + "\\" + name;
	}
	const std::string root = "\\Registry\\Machine",
						prefix = root + "\\Hardware\\Description\\System\\CentralProcessor\\";
	bool valid = name == root;
	if (name.compare(0, prefix.size(), prefix) == 0) {
		char *end;
		auto index = strtoul(name.c_str() + prefix.size(), &end, 10);
		valid = *end == 0 && index < uint32_t(sysconf(_SC_NPROCESSORS_ONLN));
	}
	if (!valid)
		return int32_t(0xc0000034);
	auto h = std::make_shared<Handle>(Handle::key);
	h->path = name;
	*out = new_handle(h);
	return 0;
}
int32_t query_value(
	void *handle, String *name, uint32_t klass, void *out, uint32_t length, uint32_t *written) {
	if (written)
		*written = 0;
	auto h = get_handle(handle);
	if (!h || h->kind != Handle::key)
		return bad_handle;
	if (!name || klass != 2)
		return not_supported;
	auto n = encode(static_cast<char16_t *>(name->buffer), name->length / 2);
	uint64_t value = 0;
	// A conservative feature model projected from Darwin's public capability
	// queries, not privileged reads of ID registers. Unexposed optional fields
	// stay absent. Unknown identification values fail instead of inventing a MIDR.
	if (n == "CP 4030") {
		value |=
			uint64_t(
				feature("hw.optional.arm.FEAT_PMULL") ? 2 : feature("hw.optional.arm.FEAT_AES"))
			<< 4;
		value |= uint64_t(feature("hw.optional.arm.FEAT_SHA1")) << 8;
		value |= uint64_t(feature("hw.optional.arm.FEAT_SHA256")) << 12;
		value |= uint64_t(feature("hw.optional.arm.FEAT_CRC32")) << 16;
		value |= uint64_t(feature("hw.optional.arm.FEAT_LSE") ? 2 : 0) << 20;
	} else if (n == "CP 4020") {
		value = 1;
		if (feature("hw.optional.arm.FEAT_FP16"))
			value |= (1ull << 16) | (1ull << 20);
	} else if (n == "CP 5801") {
		uint32_t line = 0;
		size_t size = sizeof(line);
		if (sysctlbyname("hw.cachelinesize", &line, &size, nullptr, 0) || line < 4 ||
			(line & (line - 1)))
			return not_supported;
		unsigned words_log = 0;
		for (auto words = line / 4; words > 1; words >>= 1)
			++words_log;
		value = words_log | (uint64_t(words_log) << 16);
	} else if (n == "CP 4021" || n == "CP 4031" || n == "CP 4038" || n == "CP 403A" ||
		n == "CP 4024" || n == "CP 4039" || n == "CP 4032")
		value = 0;
	else
		return int32_t(0xc0000034);
	if (written)
		*written = 20;
	if (!out || length < 20)
		return buffer_small;
	uint32_t header[3]{0, 11, 8};
	memcpy(out, header, 12);
	memcpy(static_cast<char *>(out) + 12, &value, 8);
	return 0;
}
void *resolve_registry(const char *name) {
	if (!strcmp(name, "NtOpenKeyEx"))
		return reinterpret_cast<void *>(&open_key);
	if (!strcmp(name, "NtQueryValueKey"))
		return reinterpret_cast<void *>(&query_value);
	return nullptr;
}
} // namespace RavenNT
