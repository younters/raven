#include "x86_thread_api.h"
#include "child_process.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <sys/mman.h>

namespace raven {

namespace {

constexpr std::array<std::pair<std::string_view, uint32_t>, 18> thread_exports{{
	{"CreateThread", 6}, {"DeleteCriticalSection", 1}, {"EnterCriticalSection", 1}, {"ExitThread", 1}, {"GetCurrentThreadStackLimits", 2}, {"GetExitCodeThread", 2},
	{"InitializeCriticalSection", 1}, {"InitializeCriticalSectionAndSpinCount", 2}, {"InitializeCriticalSectionEx", 3}, {"LeaveCriticalSection", 1}, {"ResumeThread", 1}, {"SetThreadPriority", 2},
	{"Sleep", 1}, {"SwitchToThread", 0}, {"TryEnterCriticalSection", 1}, {"WaitForSingleObject", 2},
	// The slow paths of the guest-side critical section stubs, which reach the runtime under these names.
	{"RavenCriticalSectionWait", 1}, {"RavenCriticalSectionWake", 1},
}};

constexpr uint32_t wait_timeout = 0x102, wait_failed = UINT32_MAX, still_active = 259;

}

NativeX86Threads::NativeX86Threads(NativeProcess &process, NativeEnvironment &environment, NativeObjects &objects, NativeThreads &threads)
	: process(process), environment(environment), objects(objects), threads(threads)
{
}

std::span<const std::pair<std::string_view, uint32_t>> NativeX86Threads::exports()
{
	return thread_exports;
}

uint32_t NativeX86Threads::wait(uint64_t handle, uint32_t milliseconds)
{
	const uint32_t id = threads.current_id();
	std::optional<std::chrono::steady_clock::time_point> wake;
	if (milliseconds != UINT32_MAX)
		wake = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
	bool looked = false;
	for (;;) {
		const int state = objects.signaled(handle, id);
		if (state < 0) {
			environment.set_last_error(state == -3 ? 5 : 6);
			return wait_failed;
		}
		if (state == 1 && objects.try_consume(handle, id)) {
			return 0;
		}
		const auto ready = [&] { return objects.signaled(handle, id) == 1; };
		if (milliseconds == 0 || (wake && std::chrono::steady_clock::now() >= *wake)) {
			// Once, a wait that has run out still takes in what the host finished meanwhile.
			if (!looked) {
				looked = true;
				if (threads.block(ready, std::chrono::steady_clock::now()))
					continue;
			}
			return wait_timeout;
		}
		// An object that turns signaled by itself, such as a timer, is looked at again when it says.
		auto until = wake;
		std::vector<pollfd> announced;
		if (const auto object = objects.object(handle)) {
			if (const auto due = object->wakes_at(); due && (!until || *due < *until))
				until = due;
			if (const int descriptor = object->wake_descriptor(); descriptor >= 0)
				announced.push_back({descriptor, POLLIN, 0});
		}
		threads.block(ready, until, announced);
	}
}

// A section is its guest fields, as on Windows, so the stubs in the guest and the calls served here share it. LockCount (4) is the lock
// word of Windows XP: -1 while free, and each entry adds one, so after the owner's entries it counts the threads waiting. OwningThread (0xc)
// and RecursionCount (8) belong to the owner. A thread that finds the section held waits here for a handoff, which a release that sees
// waiters gives: one token per release, taken by one waiter, so no thread is woken to find the section gone again.
uint32_t NativeX86Threads::section_word_at(uint64_t address)
{
	try {
		const auto bytes = process.read_memory(address, 4);
		return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
	} catch (const std::runtime_error &) {
		return 0;
	}
}

void NativeX86Threads::section_word(uint64_t address, uint32_t value)
{
	process.write_memory(address, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&value), 4));
}

int32_t NativeX86Threads::add_lock_count(uint64_t address, int32_t delta)
{
	// Atomic against the guest's own lock inc and dec on other threads; a section packed off its alignment is rare enough to take as is.
	const auto word = address + 4;
	for (;;) {
		const auto old = section_word_at(word);
		const auto wanted = uint32_t(int32_t(old) + delta);
		if (word % 4) {
			section_word(word, wanted);
			return int32_t(wanted);
		}
		if (uint32_t(process.compare_exchange(word, old, wanted, 4)) == old)
			return int32_t(wanted);
	}
}

void NativeX86Threads::wait_for_section(uint64_t address)
{
	auto &state = section_waits[address];
	++state.waiting;
	threads.block([this, address] {
		const auto found = section_waits.find(address);
		return found != section_waits.end() && found->second.handoffs > 0;
	}, std::nullopt);
	auto &after = section_waits[address];
	--after.handoffs;
	if (--after.waiting == 0 && after.handoffs == 0)
		section_waits.erase(address);
	section_word(address + 0xc, threads.current_id());
	section_word(address + 8, 1);
}

void NativeX86Threads::wake_section(uint64_t address)
{
	++section_waits[address].handoffs;
	threads.yield();
}

bool NativeX86Threads::dispatch(std::string_view name)
{
	const auto known = std::find_if(thread_exports.begin(), thread_exports.end(), [&](const auto &item) { return item.first == name; });
	if (known == thread_exports.end())
		return false;
	const auto args = process.api_arguments(known->second);
	if (name == "CreateThread") {
		const uint32_t flags = uint32_t(args[4]);
		// Security attributes only govern inheritance into child processes, which a thread handle of this process never reaches.
		if (flags & ~uint32_t(0x4 | 0x10000)) {
			environment.set_last_error(87);
			process.return_api(0);
			return true;
		}
		std::shared_ptr<GuestThread> thread;
		try {
			thread = threads.create(args[2], args[3], args[1] ? args[1] : 1024 * 1024, flags & 4);
		} catch (const std::runtime_error &) {
			environment.set_last_error(8);
			process.return_api(0);
			return true;
		}
		auto object = std::make_shared<ThreadObject>(thread);
		const auto made = objects.add_object(object);
		if (made.status) {
			threads.terminate(thread, 0);
			environment.set_last_error(8);
			process.return_api(0);
			return true;
		}
		if (args[5]) {
			const uint32_t id = thread->id;
			process.write_memory(args[5], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&id), 4));
		}
		process.return_api(made.value);
	} else if (name == "ExitThread") {
		threads.exit_current(uint32_t(args[0]));
	} else if (name == "GetExitCodeThread") {
		// A thread of another process reports through the pipe its process writes the exit code to.
		if (const auto remote = objects.object_as<RemoteThreadObject>(args[0])) {
			const uint32_t code = remote->code().value_or(still_active);
			process.write_memory(args[1], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&code), 4));
			process.return_api(1);
			return true;
		}
		// The handle table says what the handle is now: a value can be reused after the thread it named was closed.
		const auto object = objects.object_as<ThreadObject>(args[0]);
		if (!object) {
			environment.set_last_error(6);
			process.return_api(0);
			return true;
		}
		const auto &thread = *object->thread;
		const uint32_t code = thread.state == GuestThread::State::done ? thread.exit_code : still_active;
		process.write_memory(args[1], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&code), 4));
		process.return_api(1);
	} else if (name == "GetCurrentThreadStackLimits") {
		// The whole allocation, guard pages included: from DeallocationStack up to the base in the thread block.
		const auto teb = threads.current().teb;
		const auto low = process.read_memory(teb + 0xe0c, 4), high = process.read_memory(teb + 0x04, 4);
		process.write_memory(args[0], low);
		process.write_memory(args[1], high);
		process.return_api(0);
	} else if (name == "ResumeThread") {
		if (const auto child = objects.object_as<ChildThreadObject>(args[0])) {
			process.return_api(child->resume());
			return true;
		}
		const auto object = objects.object_as<ThreadObject>(args[0]);
		if (!object) {
			environment.set_last_error(6);
			process.return_api(UINT32_MAX);
			return true;
		}
		const auto previous = object->thread->suspend_count;
		threads.resume(*object->thread);
		process.return_api(uint32_t(previous));
	} else if (name == "SetThreadPriority") {
		const auto object = objects.object_as<ThreadObject>(args[0]);
		// Entering or leaving background processing mode is no priority: the thread keeps the one it has.
		const bool mode = args[1] == 0x10000 || args[1] == 0x20000;
		if (object && !mode)
			object->thread->priority = int32_t(args[1]);
		process.return_api(object != nullptr);
	} else if (name == "Sleep") {
		const uint32_t milliseconds = uint32_t(args[0]);
		if (!milliseconds) {
			threads.yield();
		} else {
			std::optional<std::chrono::steady_clock::time_point> wake;
			if (milliseconds != UINT32_MAX)
				wake = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
			threads.block([] { return false; }, wake);
		}
		process.return_api(0);
	} else if (name == "DeleteCriticalSection") {
		section_waits.erase(args[0]);
		process.return_api(0);
	} else if (name == "InitializeCriticalSection" || name == "InitializeCriticalSectionAndSpinCount" || name == "InitializeCriticalSectionEx") {
		// The high byte contains the flags; lower bits do not affect initialization.
		if (name == "InitializeCriticalSectionEx" && ((uint32_t(args[2]) & 0xff000000U) & ~0x1f000000U)) {
			environment.set_last_error(87);
			process.return_api(0);
			return true;
		}
		const bool no_debug = name == "InitializeCriticalSectionEx" && (uint32_t(args[2]) & 0x01000000U);
		if (!no_debug && !section_debug)
			section_debug = process.map_memory(4096, PROT_READ | PROT_WRITE);
		// The fields Windows keeps: DebugInfo, LockCount (-1 when free), RecursionCount, OwningThread, LockSemaphore and SpinCount.
		const uint32_t fields[6] = {no_debug ? UINT32_MAX : uint32_t(section_debug), UINT32_MAX, 0, 0, 0, name == "InitializeCriticalSection" ? 0U : uint32_t(args[1])};
		process.write_memory(args[0], std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(fields), sizeof fields));
		section_waits.erase(args[0]);
		process.return_api(name == "InitializeCriticalSection" ? 0 : 1);
	} else if (name == "EnterCriticalSection") {
		const uint32_t id = threads.current_id();
		if (add_lock_count(args[0], 1) == 0) {
			section_word(args[0] + 0xc, id);
			section_word(args[0] + 8, 1);
		} else if (section_word_at(args[0] + 0xc) == id) {
			section_word(args[0] + 8, section_word_at(args[0] + 8) + 1);
		} else {
			wait_for_section(args[0]);
		}
		process.return_api(0);
	} else if (name == "TryEnterCriticalSection") {
		const uint32_t id = threads.current_id();
		bool taken = false;
		if (!(args[0] % 4) && uint32_t(process.compare_exchange(args[0] + 4, UINT32_MAX, 0, 4)) == UINT32_MAX) {
			section_word(args[0] + 0xc, id);
			section_word(args[0] + 8, 1);
			taken = true;
		} else if (section_word_at(args[0] + 0xc) == id) {
			add_lock_count(args[0], 1);
			section_word(args[0] + 8, section_word_at(args[0] + 8) + 1);
			taken = true;
		}
		process.return_api(taken);
	} else if (name == "LeaveCriticalSection") {
		const auto depth = section_word_at(args[0] + 8) - 1;
		section_word(args[0] + 8, depth);
		if (depth == 0)
			section_word(args[0] + 0xc, 0);
		if (add_lock_count(args[0], -1) >= 0 && depth == 0)
			wake_section(args[0]);
		process.return_api(0);
	} else if (name == "RavenCriticalSectionWait") {
		// The guest's stub counted itself into a held section: this thread waits for a release to hand it over.
		wait_for_section(args[0]);
		process.return_api(0);
	} else if (name == "RavenCriticalSectionWake") {
		// A release in the guest found threads counted after it.
		wake_section(args[0]);
		process.return_api(0);
	} else if (name == "SwitchToThread") {
		process.return_api(threads.yield());
	} else {
		process.return_api(wait(args[0], uint32_t(args[1])));
	}
	return true;
}

}
