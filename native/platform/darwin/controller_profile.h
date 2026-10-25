#pragma once

#include "guest/protocol.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace raven {

// RAVEN_PROFILE_CONTROLLER=1 measures the transport separately from RAVEN_PROFILE's API handlers. All counters belong to one agent.
struct ControllerProfile {
	using Clock = std::chrono::steady_clock;
	struct Counter { uint64_t calls = 0, bytes = 0, ns = 0; };
	bool enabled = std::getenv("RAVEN_PROFILE_CONTROLLER") != nullptr;
	Counter reads, writes, shared_reads, shared_writes, mach_reads, mach_writes, waits;
	std::array<Counter, RAVEN_SERVICE_EXIT + 1> services{};
	uint64_t wait_sleeps = 0, service_sleeps = 0;

	struct Scope {
		ControllerProfile &profile;
		Counter &counter;
		size_t bytes;
		Clock::time_point start;
		Scope(ControllerProfile &profile, Counter &counter, size_t bytes = 0) : profile(profile), counter(counter), bytes(bytes),
			start(profile.enabled ? Clock::now() : Clock::time_point{}) {}
		~Scope()
		{
			if (profile.enabled) {
				++counter.calls;
				counter.bytes += bytes;
				counter.ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
			}
		}
	};

	void report() const
	{
		if (!enabled)
			return;
		const auto row = [](const char *name, const Counter &counter) {
			std::fprintf(stderr, "controller: %s calls=%llu bytes=%llu ms=%.3f us/call=%.3f\n", name,
				static_cast<unsigned long long>(counter.calls), static_cast<unsigned long long>(counter.bytes), double(counter.ns) / 1e6,
				counter.calls ? double(counter.ns) / double(counter.calls) / 1e3 : 0.0);
		};
		row("read", reads); row("write", writes);
		row("shared-read", shared_reads); row("shared-write", shared_writes);
		row("mach-read", mach_reads); row("mach-write", mach_writes);
		row("stop-wait", waits);
		static constexpr const char *names[] = {"none", "map", "protect", "unmap", "discard", "map-descriptor", "set-ldt", "read", "write",
			"available", "flush", "exit"};
		for (size_t index = 0; index < services.size(); ++index)
			if (services[index].calls) {
				char label[64];
				std::snprintf(label, sizeof label, "service-%s", names[index]);
				row(label, services[index]);
			}
		std::fprintf(stderr, "controller: stop-sleeps=%llu service-sleeps=%llu\n", static_cast<unsigned long long>(wait_sleeps),
			static_cast<unsigned long long>(service_sleeps));
	}
};

}
