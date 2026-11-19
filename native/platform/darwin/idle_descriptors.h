#pragma once

#include <map>
#include <poll.h>
#include <vector>

namespace raven {

inline void darwin_merge_idle_descriptors(std::vector<pollfd> &descriptors)
{
	// Darwin poll observes only the last entry for a repeated fd. Preserve the union without moving entries: desktop callers
	// retain indices into this vector.
	std::map<int, size_t> first;
	for (size_t index = 0; index < descriptors.size(); ++index) {
		auto &descriptor = descriptors[index];
		if (descriptor.fd < 0) continue;
		const auto [position, added] = first.emplace(descriptor.fd, index);
		if (!added) {
			descriptors[position->second].events |= descriptor.events;
			descriptor.fd = -1;
		}
	}
}

}
