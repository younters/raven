// SPDX-License-Identifier: GPL-3.0-or-later
#include "commit.h"
#include "desktop/windows.h"
#include "nt/text.h"
#include <cmath>
#include <cstring>
#include <set>
#include <utility>

namespace raven {

uint32_t darwin::publish_composition(DesktopObjects &objects, uint64_t device, uint32_t magic, uint32_t count,
	std::span<const raven_dcomp_item> items, const std::function<bool(uint64_t)> &foreign_target)
{
	if (magic != RAVEN_DCOMP_COMMIT_MAGIC || count > RAVEN_DCOMP_MAX_VISUALS || items.size() != count)
		return 0x80070057U;
	std::set<uint64_t> streams;
	for (const auto &item : items) {
		const auto *window = std::as_const(objects).window(item.stream);
		if (!window || !window->properties.contains(u"RAVENDCOMPSWAPCHAIN") || !streams.insert(item.stream).second ||
			!item.target || (!objects.window(item.target) && !foreign_target(item.target)) ||
			!std::isfinite(item.x) || !std::isfinite(item.y) || !std::isfinite(item.sx) || !std::isfinite(item.sy) ||
			item.sx <= 0 || item.sy <= 0 || item.sx > 16384 || item.sy > 16384 || item.linear > 1) {
			return 0x80070057U;
		}
		for (const float value : item.clip)
			if (!std::isfinite(value))
				return 0x80070057U;
		for (const float value : item.rounded)
			if (!std::isfinite(value))
				return 0x80070057U;
		for (const float value : item.radius)
			if (!std::isfinite(value) || value < 0)
				return 0x80070057U;
	}
	// No guest calls, callbacks, or presentation polls occur while the transaction
	// is published. Pending setters remain entirely in the PE component.
	for (const auto handle : objects.window_handles()) {
		auto *window = objects.window(handle);
		const auto owner = window->properties.find(u"RAVENDCOMPDEVICE");
		if (owner != window->properties.end() && owner->second == device)
			window->properties[u"RAVENDCOMPTARGET"] = 0;
	}
	for (const auto &item : items) {
		auto &properties = objects.window(item.stream)->properties;
		const auto number = [&](const char16_t *name, float value) {
			uint32_t bits;
			std::memcpy(&bits, &value, 4);
			properties[name] = bits;
		};
		properties[u"RAVENDCOMPDEVICE"] = device;
		properties[u"RAVENDCOMPTARGET"] = item.target;
		number(u"RAVENDCOMPX", item.x);
		number(u"RAVENDCOMPY", item.y);
		number(u"RAVENDCOMPSX", item.sx);
		number(u"RAVENDCOMPSY", item.sy);
		number(u"RAVENDCOMPCLIPL", item.clip[0]);
		number(u"RAVENDCOMPCLIPT", item.clip[1]);
		number(u"RAVENDCOMPCLIPR", item.clip[2]);
		number(u"RAVENDCOMPCLIPB", item.clip[3]);
		properties[u"RAVENDCOMPZ"] = uint32_t(item.order);
		properties[u"RAVENDCOMPLINEAR"] = item.linear;
		for (size_t i = 0; i < 4; ++i) {
			uint32_t bits;
			std::memcpy(&bits, &item.rounded[i], 4);
			properties[u"RAVENDCOMPROUND" + widen_ascii(std::to_string(i))] = bits;
		}
		for (size_t i = 0; i < 8; ++i) {
			uint32_t bits;
			std::memcpy(&bits, &item.radius[i], 4);
			properties[u"RAVENDCOMPRADIUS" + widen_ascii(std::to_string(i))] = bits;
		}
	}
	return 0;
}

bool NativeWindows::commit_composition(ApiCall &call) {
	const auto header = call.read(call[2], 8);
	uint32_t magic = 0, count = 0;
	std::memcpy(&magic, header.data(), 4);
	std::memcpy(&count, header.data() + 4, 4);
	if (magic != RAVEN_DCOMP_COMMIT_MAGIC || count > RAVEN_DCOMP_MAX_VISUALS) {
		call.set_last_error(87);
		return false;
	}
	const auto bytes = call.read(call[2] + 8, size_t(count) * sizeof(raven_dcomp_item));
	std::vector<raven_dcomp_item> items(count);
	if (!bytes.empty())
		std::memcpy(items.data(), bytes.data(), bytes.size());
	if (darwin::publish_composition(objects, call[0], magic, count, items,
		[](uint64_t target) { return bool(foreign_window_info(target)); })) {
		call.set_last_error(87);
		return false;
	}
	return true;
}

std::optional<darwin::CompositionBinding> NativeWindows::composition_binding(uint64_t handle) const {
	const auto *window = objects.window(handle);
	if (!window || !window->properties.contains(u"RAVENDCOMPSWAPCHAIN"))
		return std::nullopt;
	const auto get = [&](const char16_t *name) {
		auto it = window->properties.find(name);
		return it == window->properties.end() ? uint64_t(0) : it->second;
	};
	const auto number = [&](const char16_t *name) {
		uint32_t bits = uint32_t(get(name));
		float f;
		std::memcpy(&f, &bits, 4);
		return f;
	};
	auto binding = darwin::CompositionBinding{
		get(u"RAVENDCOMPTARGET"),	number(u"RAVENDCOMPX"),	   number(u"RAVENDCOMPY"),	   int32_t(get(u"RAVENDCOMPZ")),
		number(u"RAVENDCOMPSX"),	number(u"RAVENDCOMPSY"),   number(u"RAVENDCOMPCLIPL"), number(u"RAVENDCOMPCLIPT"),
		number(u"RAVENDCOMPCLIPR"), number(u"RAVENDCOMPCLIPB")};
	binding.linear = get(u"RAVENDCOMPLINEAR") != 0;
	for (size_t i = 0; i < binding.rounded.size(); ++i) {
		const auto key = u"RAVENDCOMPROUND" + widen_ascii(std::to_string(i));
		binding.rounded[i] = number(key.c_str());
	}
	for (size_t i = 0; i < binding.radius.size(); ++i) {
		const auto key = u"RAVENDCOMPRADIUS" + widen_ascii(std::to_string(i));
		binding.radius[i] = number(key.c_str());
	}
	// A GPU process owns its compositor child; the browser owns that child's parent.
	// Positions above that foreign boundary are relative to the browser client area.
	for (unsigned depth = 0; depth < 64; ++depth) {
		const auto *node = objects.window(binding.target);
		if (!node || !node->parent)
			break;
		if (!objects.window(node->parent) && foreign_window_info(node->parent)) {
			binding.x += node->x;
			binding.y += node->y;
			binding.left += node->x;
			binding.top += node->y;
			binding.right += node->x;
			binding.bottom += node->y;
			binding.rounded[0] += node->x;
			binding.rounded[2] += node->x;
			binding.rounded[1] += node->y;
			binding.rounded[3] += node->y;
			binding.target = node->parent;
			break;
		}
		binding.target = node->parent;
	}
	return binding;
}

}
