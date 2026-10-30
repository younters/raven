// Generous aggregate bounds distinguish descriptor wakes from the old 20 ms scheduler sweep.
#include "nt/providers.h"
#include "nt/threads.h"
#include "desktop/windows.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>

static void check(bool value, const char *message)
{
	if (!value) throw std::runtime_error(message);
}

int main(int argc, char **argv)
{
	try {
		check(argc == 2, "bootstrap required");
		raven::ModuleSet modules;
		const std::array<std::string_view, 1> names{"GetLastError"};
		auto &image = modules.add("kernel32.dll", raven::runtime_provider("kernel32.dll", 0x180000000ULL, names));
		raven::LoadedImage *images[]{&image.loaded};
		raven::NativeProcess process(images, argv[1], std::chrono::seconds(10));
		raven::NativeEnvironment environment(process, image.loaded.base(), 1, 1);
		raven::NativeMemory memory(process, environment);
		raven::NativeThreads threads(process, environment, memory, nullptr, std::chrono::steady_clock::now() + std::chrono::seconds(10));
		int ends[2];
		check(socketpair(AF_UNIX, SOCK_STREAM, 0, ends) == 0, "socketpair failed");
		struct Close { int *ends; ~Close() { close(ends[0]); close(ends[1]); } } close_pair{ends};
		std::array<long long, 30> samples;
		for (auto &sample : samples) {
			std::chrono::steady_clock::time_point sent;
			std::jthread sender([&] {
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
				sent = std::chrono::steady_clock::now();
				const char byte = 1;
				write(ends[1], &byte, 1);
			});
			const auto ready = [&] { pollfd fd{ends[0], POLLIN, 0}; return ::poll(&fd, 1, 0) > 0; };
			const std::array<pollfd, 2> watched{{{ends[0], POLLIN, 0}, {ends[0], POLLIN, 0}}};
			const bool satisfied = threads.block(ready, std::chrono::steady_clock::now() + std::chrono::seconds(2), watched);
			const auto resumed = std::chrono::steady_clock::now();
			sender.join();
			check(satisfied, "socket readiness wait failed");
			sample = std::chrono::duration_cast<std::chrono::microseconds>(resumed - sent).count();
			char byte;
			check(read(ends[0], &byte, 1) == 1, "read failed");
		}
		std::sort(samples.begin(), samples.end());
		std::cout << "socket wake count " << samples.size() << " p50_us " << samples[15] << " p95_us " << samples[28] << '\n';
		check(samples[15] < 5000, "median socket wake exceeded 5 ms");
		check(samples[28] < 15000, "p95 socket wake exceeded 15 ms");
		// Inspect the actual desktop/scheduler snapshot: duplicates are merged and the lease stays alive through polling.
		bool inspected = false;
		threads.set_idle([&](auto) {
			std::vector<pollfd> descriptors;
			const auto lease = threads.idle_descriptors(descriptors);
			unsigned matches = 0;
			for (const auto &fd : descriptors)
				if (fd.fd == ends[0]) { ++matches; check(fd.events == (POLLIN | POLLOUT), "duplicate masks lost"); }
			check(matches == 1, "wait descriptor not merged into idle snapshot");
			inspected = true;
		});
		const std::array<pollfd, 2> watched{{{ends[0], POLLIN, 0}, {ends[0], POLLOUT, 0}}};
		check(!threads.block([] { return false; }, std::chrono::steady_clock::now() + std::chrono::milliseconds(5), watched),
			"spurious readiness bypassed predicate");
		check(inspected, "idle callback did not run");
		std::vector<pollfd> after;
		const auto lease = threads.idle_descriptors(after);
		for (const auto &fd : after) check(fd.fd != ends[0], "completed wait retained borrowed descriptor");
		// A nested desktop poll must restore the outer registration, including during exception unwinding.
		raven::DesktopObjects desktop;
		raven::NativeWindows windows(process, environment, desktop);
		std::vector<pollfd> active;
		unsigned depth = 0;
		raven::DesktopPresentation backend;
		backend.wait_descriptors = [&](std::span<const pollfd> descriptors) { active.assign(descriptors.begin(), descriptors.end()); };
		backend.poll = [&](auto) {
			if (depth++ == 0) {
				windows.poll_idle(std::chrono::milliseconds(0));
				check(active.size() == 1 && active[0].fd == ends[0], "nested poll disarmed outer wait");
			}
			--depth;
			return std::vector<raven::DesktopEvent>{};
		};
		windows.set_presentation(backend);
		windows.set_idle_sources([](auto limit) { return limit; }, [&](std::vector<pollfd> &descriptors) {
			descriptors.push_back({ends[depth ? 1 : 0], POLLIN, 0});
			return std::shared_ptr<void>{};
		}, [] {});
		windows.poll_idle(std::chrono::milliseconds(0));
		check(active.empty(), "outer poll retained its registration");
		backend.poll = [](auto) -> std::vector<raven::DesktopEvent> { throw std::runtime_error("poll interrupted"); };
		windows.set_presentation(backend);
		bool interrupted = false;
		try { windows.poll_idle(std::chrono::milliseconds(0)); }
		catch (const std::runtime_error &) { interrupted = true; }
		check(interrupted && active.empty(), "exception retained an idle registration");
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
