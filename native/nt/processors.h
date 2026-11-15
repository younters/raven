#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace raven {

// The logical processors GetSystemInfo tells a program about, and that the calls taking a processor number accept: the host's, in one
// group, at least one and at most as many as a mask the width of the program's pointers holds (32 for a 32-bit program, 64 otherwise).
inline uint32_t reported_processors(size_t pointer_width)
{
	return std::max(1U, std::min(pointer_width == 4 ? 32U : 64U, std::thread::hardware_concurrency()));
}

// The mask with a bit for each of those processors.
inline uint64_t reported_processor_mask(size_t pointer_width)
{
	const uint32_t count = reported_processors(pointer_width);
	return count >= 64 ? ~uint64_t(0) : (uint64_t(1) << count) - 1;
}

}
