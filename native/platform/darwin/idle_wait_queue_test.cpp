#include "idle_wait_queue.h"
#include <stdexcept>
#include <iostream>
#include <limits>

static void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
static bool ready(int descriptor) { pollfd watch{descriptor, POLLIN, 0}; return ::poll(&watch, 1, 0) == 1 && (watch.revents & POLLIN); }
int main()
{
	try {
		raven::DarwinIdleWaitQueue queue;
		check(queue.descriptor() >= 0, "kqueue failed");
		int a[2], b[2];
		check(pipe(a) == 0 && pipe(b) == 0, "pipe failed");
		struct Close { int *a, *b; ~Close() { for (int i = 0; i < 2; ++i) { close(a[i]); close(b[i]); } } } close_pipes{a, b};
		pollfd first{a[0], POLLIN, 0}, second{b[0], POLLIN, 0};
		check(queue.arm({&first, 1}), "arm failed");
		check(!ready(queue.descriptor()), "empty pipe was ready");
		const char byte = 1;
		check(write(a[1], &byte, 1) == 1 && ready(queue.descriptor()), "pipe did not wake kqueue");
		check(queue.consume() && !ready(queue.descriptor()), "one-shot readiness repeated");
		check(queue.arm({&first, 1}) && ready(queue.descriptor()), "still-readable descriptor did not rearm");
		check(queue.arm({&second, 1}) && !ready(queue.descriptor()), "previous wait left stale readiness");
		check(write(b[1], &byte, 1) == 1 && ready(queue.descriptor()), "replacement wait did not wake");
		queue.disarm();
		check(!ready(queue.descriptor()), "disarm retained readiness");
		// Close and reuse the same descriptor number. Every arm must add a fresh kernel registration.
		const int reused = b[0];
		close(b[0]);
		int replacement[2];
		check(pipe(replacement) == 0, "replacement pipe failed");
		if (replacement[0] != reused) { check(dup2(replacement[0], reused) == reused, "dup2 failed"); close(replacement[0]); }
		check(queue.arm({&second, 1}) && write(replacement[1], &byte, 1) == 1 && ready(queue.descriptor()), "reused descriptor lost its wake");
		close(replacement[1]);
		queue.disarm();
		const std::array<pollfd, 2> invalid{{first, {std::numeric_limits<int>::max(), POLLIN, 0}}};
		check(!queue.arm(invalid) && !ready(queue.descriptor()), "failed arm retained partial registrations");
		check(queue.arm({&first, 1}) && ready(queue.descriptor()), "failed arm prevented a later valid wait");
		queue.disarm();
		char received = 0;
		check(read(a[0], &received, 1) == 1 && received == byte, "queue consumed borrowed descriptor data");
		const pollfd exceptional{a[0], POLLPRI, 0};
		check(!queue.arm({&exceptional, 1}), "except-only wait did not select fallback");
		const std::array<pollfd, 2> mixed{{first, exceptional}};
		check(!queue.arm(mixed) && !ready(queue.descriptor()), "unsupported mask left partial registrations");
		check(queue.arm({&first, 1}), "unsupported mask prevented later valid wait");
		queue.disarm();
		check(fcntl(queue.descriptor(), F_GETFD) & FD_CLOEXEC, "queue would leak across exec");
		return 0;
	} catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
