#pragma once

#include "environment.h"
#include "objects.h"
#include "threads.h"

#include <map>

namespace raven {

// Thread and wait services of a 32-bit process: stdcall arguments are read from the guest stack.
class NativeX86Threads {
public:
	NativeX86Threads(NativeProcess &process, NativeEnvironment &environment, NativeObjects &objects, NativeThreads &threads);
	bool dispatch(std::string_view name);
	static std::span<const std::pair<std::string_view, uint32_t>> exports();

private:
	NativeProcess &process;
	NativeEnvironment &environment;
	NativeObjects &objects;
	NativeThreads &threads;
#ifdef __APPLE__
	// Threads waiting here for each critical section and the handoffs releases have given them, by guest address.
	struct SectionWait {
		uint32_t waiting = 0;
		uint32_t handoffs = 0;
	};
	std::map<uint64_t, SectionWait> section_waits;
#else
	struct Section {
		uint32_t owner = 0;
		uint32_t depth = 0;
	};
	// Critical sections are keyed by guest address: threads switch only inside API calls, so no host lock is needed.
	std::map<uint64_t, Section> sections;
#endif
	// The debug data every section points at: callers that force it write the section's name there.
	uint64_t section_debug = 0;
	uint32_t wait(uint64_t handle, uint32_t milliseconds);
#ifdef __APPLE__
	uint32_t section_word_at(uint64_t address);
	int32_t add_lock_count(uint64_t address, int32_t delta);
	void section_word(uint64_t address, uint32_t value);
	void wait_for_section(uint64_t address);
	void wake_section(uint64_t address);
#else
	// Writes the owner fields of the guest's section as Windows keeps them, which code that tests a section inline reads.
	void mirror_section(uint64_t address, const Section &section);
#endif
};

}
