// The macOS guest: an x86_64 program that Rosetta 2 translates, in which Windows code runs (see protocol.h for the control block).
//
// The main thread is the guest thread. It stops once at startup and from then on only runs the contexts the host gives it: every way
// Windows code can stop (a breakpoint, a fault, a system call, the host's interrupt) is a signal, whose handler publishes the registers,
// waits for the host and returns into the registers it finds. In parallel mode more pthreads do the same, each with its own channel
// and signal stack. A service thread handles memory and pthread operations, and a watcher ends the guest when the host goes away.

#include "protocol.h"

#include <architecture/i386/table.h>
#include <dlfcn.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <i386/user_ldt.h>
#include <mach/mach.h>
#include <os/os_sync_wait_on_address.h>
#include <pthread.h>
#include <servers/bootstrap.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/ucontext.h>
#include <unistd.h>

static struct raven_control *control;
static uint8_t *data_area;

// The guest thread's pthread thread-specific data, which GS addresses in macOS code; Windows x64 code runs with GS at its TEB instead.
uint64_t raven_agent_tsds[RAVEN_GUEST_MAX_THREADS];
// The GS base the guest runs with now: its TEB for 64-bit code, the agent's own for 32-bit code, which does not use GS.
static uint64_t guest_gs_bases[RAVEN_GUEST_MAX_THREADS];
static pthread_t guest_threads[RAVEN_GUEST_MAX_THREADS];
// Published before a pthread enters PE code. The gate finds its record with thread_selfid, which needs no Darwin TSD.
struct unix_thread {
	uint64_t identity;
	void *stack_top;
	uint64_t tsd;
	uint64_t *guest_gs;
	uint32_t *active;
	uint64_t reserved[3];
};
struct unix_thread raven_unix_threads[RAVEN_GUEST_MAX_THREADS];
_Static_assert(sizeof(struct unix_thread) == 64, "unix gate record stride");
extern void raven_unix_call_gate(void), raven_unix_call_gate_end(void);
extern void raven_unix_call_native_begin(void), raven_unix_call_native_end(void);

// Only host-provisioned libraries are loaded. Keep them alive until process exit: PE code can retain table and object pointers.
struct unix_library {
	void *module;
	int32_t (**functions)(void *);
	uint32_t count;
};
static struct unix_library unix_libraries[32];
static uint32_t unix_library_count;

int32_t raven_dispatch_unix_call(uint64_t handle, uint32_t code, void *args)
{
	int32_t result = (int32_t)0xc0000008U; // STATUS_INVALID_HANDLE
	const uint32_t count = __atomic_load_n(&unix_library_count, __ATOMIC_ACQUIRE);
	for (uint32_t i = 0; i < count; ++i) {
		const struct unix_library *library = &unix_libraries[i];
		if ((uint64_t)library->functions != handle)
			continue;
		result = (int32_t)0xc000000dU; // STATUS_INVALID_PARAMETER
		if (code < library->count && library->functions[code])
			result = library->functions[code](args);
		break;
	}
	return result;
}

static int inside_unix_call(uint64_t rip, uint32_t index)
{
	return __atomic_load_n(&control->threads[index].unix_active, __ATOMIC_ACQUIRE) ||
		(rip >= (uint64_t)raven_unix_call_gate && rip < (uint64_t)raven_unix_call_gate_end);
}

static uint64_t interrupted_gs_base(uint64_t rip, uint32_t index)
{
	// Rosetta reports zero in __gsbase for native code even after thread_fast_set_cthread_self64. Track both transitions ourselves.
	if (rip >= (uint64_t)raven_unix_call_gate && rip < (uint64_t)raven_unix_call_gate_end)
		return rip >= (uint64_t)raven_unix_call_native_begin && rip < (uint64_t)raven_unix_call_native_end ?
			raven_agent_tsds[index] : guest_gs_bases[index];
	return __atomic_load_n(&control->threads[index].unix_active, __ATOMIC_ACQUIRE) ? raven_agent_tsds[index] : guest_gs_bases[index];
}
// The guest thread's signal stack: Windows code owns its stack, and the handler must run where Windows code cannot reach.
#define ALTSTACK_SIZE (256 * 1024)
// Equal-sized stacks let the entry stub find its TSD without calling libc with a Windows GS base.
char raven_agent_altstacks[RAVEN_GUEST_MAX_THREADS][ALTSTACK_SIZE] __attribute__((aligned(64)));

// The base of every LDT data descriptor the host made, for reporting FS bases.
#define LDT_ENTRIES 8192
static uint32_t ldt_bases[LDT_ENTRIES];

// The low 4 GiB, a page per bit: set where the host has mapped memory, clear where the zero-fill reserve still holds the range.
#define LOW_PAGES ((RAVEN_LOW_END - RAVEN_LOW_START) / 4096)
static uint64_t low_used[(LOW_PAGES + 63) / 64];

static pthread_t service_thread;
static sigjmp_buf service_recovery;
static volatile sig_atomic_t service_guarded;

static void fail(const char *what)
{
	fprintf(stderr, "raven-bootstrap: %s: %s\n", what, strerror(errno));
	_exit(125);
}

// ---- waiting on shared words

static uint32_t load(uint32_t *word)
{
	return __atomic_load_n(word, __ATOMIC_ACQUIRE);
}

static void store(uint32_t *word, uint32_t value)
{
	__atomic_store_n(word, value, __ATOMIC_RELEASE);
	os_sync_wake_by_address_all(word, sizeof *word, OS_SYNC_WAKE_BY_ADDRESS_SHARED);
}

// Spins a little, since the host usually answers within microseconds, then sleeps until the word changes from `value`.
static uint32_t wait_change(uint32_t *word, uint32_t value)
{
	for (int spin = 0; spin < 4000; spin++) {
		const uint32_t now = load(word);
		if (now != value)
			return now;
		__builtin_ia32_pause();
	}
	for (;;) {
		const uint32_t now = load(word);
		if (now != value)
			return now;
		os_sync_wait_on_address_with_timeout(word, value, sizeof *word, OS_SYNC_WAIT_ON_ADDRESS_SHARED, OS_CLOCK_MACH_ABSOLUTE_TIME,
			100 * 1000 * 1000);
	}
}

// ---- GS

static long set_gs_base(uint64_t base)
{
	long result;
	// thread_fast_set_cthread_self64: the machine-dependent call that sets this thread's GS base.
	__asm__ volatile("syscall" : "=a"(result) : "a"(0x3000003L), "D"(base) : "rcx", "r11", "memory");
	return result;
}

// The address GS points at for macOS code: the thread's TSD array, which lies inside its pthread structure. Slot 0 holds the pthread
// pointer, so the array is found where the first slots read through GS sit in that structure. Rosetta offers no RDGSBASE.
static uint64_t find_tsd_base(void)
{
	uint64_t slots[4];
	for (uint64_t index = 0; index < 4; index++)
		__asm__ volatile("movq %%gs:(%1), %0" : "=r"(slots[index]) : "r"(index * 8));
	const uint8_t *self = (const uint8_t *)slots[0];
	for (uint64_t offset = 0; offset < 8192; offset += 8) {
		const uint64_t *candidate = (const uint64_t *)(self + offset);
		if (candidate[0] == slots[0] && candidate[1] == slots[1] && candidate[2] == slots[2] && candidate[3] == slots[3])
			return (uint64_t)candidate;
	}
	errno = ESRCH;
	fail("cannot find the thread's TSD");
	return 0;
}

// ---- the low reserve

static int low_page(uint64_t address, uint64_t *index)
{
	if (address < RAVEN_LOW_START || address >= RAVEN_LOW_END)
		return 0;
	*index = (address - RAVEN_LOW_START) / 4096;
	return 1;
}

static int low_used_page(uint64_t index)
{
	return (low_used[index / 64] >> (index % 64)) & 1;
}

static void mark_low(uint64_t address, uint64_t size, int used)
{
	for (uint64_t at = address; at < address + size; at += 4096) {
		uint64_t index;
		if (!low_page(at, &index))
			continue;
		if (used)
			low_used[index / 64] |= 1ULL << (index % 64);
		else
			low_used[index / 64] &= ~(1ULL << (index % 64));
	}
}

// Whether nothing the host made lies in the low part of the range, and no foreign mapping in the part above it.
static int range_free(uint64_t address, uint64_t size)
{
	const uint64_t end = address + size;
	if (address < RAVEN_LOW_START || end < address)
		return 0;
	for (uint64_t at = address; at < end && at < RAVEN_LOW_END; at += 4096) {
		uint64_t index;
		low_page(at, &index);
		if (low_used_page(index))
			return 0;
	}
	if (end > RAVEN_LOW_END) {
		const uint64_t high = address > RAVEN_LOW_END ? address : RAVEN_LOW_END;
		// Above the reserve, the kernel decides: asked for the address without insisting, it gives it only when nothing is there.
		void *probe = mmap((void *)high, end - high, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (probe == MAP_FAILED)
			return 0;
		munmap(probe, end - high);
		if ((uint64_t)probe != high)
			return 0;
	}
	return 1;
}

// The lowest free range of the reserve on a 64 KiB boundary.
static uint64_t low_gap(uint64_t size)
{
	const uint64_t pages = (size + 4095) / 4096;
	uint64_t start = 0, run = 0;
	for (uint64_t index = 0; index < LOW_PAGES; index++) {
		if (!run) {
			const uint64_t address = RAVEN_LOW_START + index * 4096;
			if (address % 0x10000) {
				continue;
			}
			start = index;
		}
		if (low_used_page(index)) {
			run = 0;
			continue;
		}
		if (++run == pages)
			return RAVEN_LOW_START + start * 4096;
	}
	return 0;
}

// Gives part of the low range back to the reserve: no-access memory nothing else may be placed in.
static int release_low(uint64_t address, uint64_t size)
{
	if (mmap((void *)address, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED)
		return -1;
	mark_low(address, size, 0);
	return 0;
}

static int release(uint64_t address, uint64_t size)
{
	const uint64_t end = address + size;
	if (address < RAVEN_LOW_END) {
		const uint64_t low_end = end < RAVEN_LOW_END ? end : RAVEN_LOW_END;
		if (release_low(address, low_end - address) != 0)
			return -1;
	}
	if (end > RAVEN_LOW_END) {
		const uint64_t high = address > RAVEN_LOW_END ? address : RAVEN_LOW_END;
		if (munmap((void *)high, end - high) != 0)
			return -1;
	}
	return 0;
}

// ---- service operations

// Chooses where a mapping goes: exactly at a free address, the lowest free 64 KiB boundary of the reserve, or wherever the kernel puts it
// (on a 64 KiB boundary when asked). Returns 0 with errno set when there is no such place.
static uint64_t place(uint64_t address, uint64_t size, uint64_t flags)
{
	if (flags & RAVEN_MAP_EXACT) {
		if (address % 4096 || !range_free(address, size)) {
			errno = EEXIST;
			return 0;
		}
		return address;
	}
	if (flags & RAVEN_MAP_LOW) {
		const uint64_t gap = low_gap(size);
		if (!gap)
			errno = ENOMEM;
		return gap;
	}
	const uint64_t slack = (flags & RAVEN_MAP_GRANULAR) ? 0x10000 : 0;
	void *reserved = mmap(NULL, size + slack, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (reserved == MAP_FAILED)
		return 0;
	const uint64_t start = (uint64_t)reserved;
	const uint64_t aligned = slack ? (start + slack - 1) & ~(slack - 1) : start;
	if (aligned > start)
		munmap(reserved, aligned - start);
	if (start + size + slack > aligned + size)
		munmap((void *)(aligned + size), start + size + slack - aligned - size);
	return aligned;
}

static uint64_t map_anonymous(uint64_t address, uint64_t size, int protection, uint64_t flags)
{
	const uint64_t at = place(address, size, flags);
	if (!at)
		return 0;
	if (mmap((void *)at, size, protection, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED)
		return 0;
	mark_low(at, size, 1);
	return at;
}

static int receive_descriptor(void)
{
	char byte;
	struct iovec vector = {&byte, 1};
	union {
		struct cmsghdr header;
		char space[CMSG_SPACE(sizeof(int))];
	} control_data;
	struct msghdr message = {0};
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	message.msg_control = &control_data;
	message.msg_controllen = sizeof control_data;
	ssize_t received;
	do {
		received = recvmsg(RAVEN_GUEST_SOCKET_FD, &message, 0);
	} while (received < 0 && errno == EINTR);
	struct cmsghdr *header = CMSG_FIRSTHDR(&message);
	if (received != 1 || !header || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) {
		errno = EPROTO;
		return -1;
	}
	int descriptor;
	memcpy(&descriptor, CMSG_DATA(header), sizeof descriptor);
	fcntl(descriptor, F_SETFD, FD_CLOEXEC);
	return descriptor;
}

static uint64_t map_descriptor(uint64_t address, uint64_t size, uint64_t offset, uint64_t flags, int protection)
{
	const int descriptor = receive_descriptor();
	if (descriptor < 0)
		return 0;
	uint64_t at = place(address, size, flags);
	if (at && mmap((void *)at, size, protection, MAP_SHARED | MAP_FIXED, descriptor, (off_t)offset) == MAP_FAILED) {
		const int error = errno;
		if (!(flags & (RAVEN_MAP_EXACT | RAVEN_MAP_LOW)) || at < RAVEN_LOW_END)
			release(at, size);
		errno = error;
		at = 0;
	}
	if (at)
		mark_low(at, size, 1);
	const int error = errno;
	close(descriptor);
	errno = error;
	return at;
}

static int set_ldt(uint32_t index, uint64_t base, uint64_t limit, int code)
{
	if (index < RAVEN_LDT_CODE32 || index >= LDT_ENTRIES) {
		errno = EINVAL;
		return -1;
	}
	union ldt_entry entry;
	memset(&entry, 0, sizeof entry);
	const int pages = limit > 0xfffff;
	const uint64_t units = pages ? limit >> 12 : limit;
	if (code) {
		entry.code.limit00 = (uint16_t)units;
		entry.code.limit16 = (uint8_t)(units >> 16) & 0xf;
		entry.code.base00 = (uint16_t)base;
		entry.code.base16 = (uint8_t)(base >> 16);
		entry.code.base24 = (uint8_t)(base >> 24);
		entry.code.type = DESC_CODE_READ;
		entry.code.dpl = 3;
		entry.code.present = 1;
		entry.code.opsz = DESC_CODE_32B;
		entry.code.granular = pages ? DESC_GRAN_PAGE : DESC_GRAN_BYTE;
	} else {
		entry.data.limit00 = (uint16_t)units;
		entry.data.limit16 = (uint8_t)(units >> 16) & 0xf;
		entry.data.base00 = (uint16_t)base;
		entry.data.base16 = (uint8_t)(base >> 16);
		entry.data.base24 = (uint8_t)(base >> 24);
		entry.data.type = DESC_DATA_WRITE;
		entry.data.dpl = 3;
		entry.data.present = 1;
		entry.data.stksz = DESC_DATA_32B;
		entry.data.granular = pages ? DESC_GRAN_PAGE : DESC_GRAN_BYTE;
	}
	if (i386_set_ldt((int)index, &entry, 1) < 0)
		return -1;
	ldt_bases[index] = (uint32_t)base;
	return 0;
}

// Copies between guest memory and the data area; a fault on the way is reported, not taken for the guest's.
static int copy_guarded(void *to, const void *from, size_t size)
{
	if (sigsetjmp(service_recovery, 1)) {
		service_guarded = 0;
		errno = EFAULT;
		return -1;
	}
	service_guarded = 1;
	memcpy(to, from, size);
	service_guarded = 0;
	return 0;
}

static int write_guest(uint64_t address, uint64_t size, int protection)
{
	// Stores of the guest's own let Rosetta see code change; pages that do not take them are opened for the copy and closed again,
	// which also has Rosetta translate what they hold afresh.
	const uint64_t first = address & ~4095ULL, last = (address + size + 4095) & ~4095ULL;
	const int reopen = !(protection & PROT_WRITE) || (protection & PROT_EXEC);
	if (reopen && mprotect((void *)first, last - first, PROT_READ | PROT_WRITE) != 0)
		return -1;
	const int copied = copy_guarded((void *)address, data_area, size);
	const int error = errno;
	if (reopen && mprotect((void *)first, last - first, protection) != 0)
		return -1;
	errno = error;
	return copied;
}

static void *guest_start(void *raw);

static void interrupt_thread(uint32_t index)
{
	if (load(&control->threads[index].state) == RAVEN_THREAD_STOPPED)
		return;
	const int error = pthread_kill(guest_threads[index], SIGUSR1);
	if (error) {
		errno = error;
		fail("cannot interrupt guest thread");
	}
}

// Unlike a host read followed by a write, these locked x86 operations are atomic with translated Windows code too.
static uint64_t compare_exchange(uint64_t address, uint64_t expected, uint64_t desired, uint64_t size)
{
	if (sigsetjmp(service_recovery, 1)) {
		service_guarded = 0;
		errno = EFAULT;
		return 0;
	}
	service_guarded = 1;
	if (size == 4) {
		uint32_t previous = (uint32_t)expected;
		__atomic_compare_exchange_n((uint32_t *)address, &previous, (uint32_t)desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
		expected = previous;
	} else {
		__atomic_compare_exchange_n((uint64_t *)address, &expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	}
	service_guarded = 0;
	return expected;
}

static void serve(struct raven_service_channel *service)
{
	const uint64_t *a = service->arguments;
	uint64_t result = 0;
	int ok = 1;
	errno = 0;
	switch (service->command) {
	case RAVEN_SERVICE_MAP:
		result = map_anonymous(a[0], a[1], (int)a[2], a[3]);
		ok = result != 0;
		break;
	case RAVEN_SERVICE_PROTECT:
		ok = mprotect((void *)a[0], a[1], (int)a[2]) == 0;
		break;
	case RAVEN_SERVICE_UNMAP:
		ok = release(a[0], a[1]) == 0;
		break;
	case RAVEN_SERVICE_DISCARD:
		if (a[3])
			ok = madvise((void *)a[0], a[1], MADV_DONTNEED) == 0 || errno == EINVAL;
		else
			ok = mmap((void *)a[0], a[1], (int)a[2], MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != MAP_FAILED;
		break;
	case RAVEN_SERVICE_MAP_DESCRIPTOR:
		result = map_descriptor(a[0], a[1], a[2], a[3], (int)a[4]);
		ok = result != 0;
		break;
	case RAVEN_SERVICE_SET_LDT:
		ok = set_ldt((uint32_t)a[0], a[1], a[2], (int)a[3]) == 0;
		break;
	case RAVEN_SERVICE_READ:
		ok = a[1] <= RAVEN_CONTROL_DATA_SIZE && copy_guarded(data_area, (const void *)a[0], a[1]) == 0;
		break;
	case RAVEN_SERVICE_WRITE:
		ok = a[1] <= RAVEN_CONTROL_DATA_SIZE && write_guest(a[0], a[1], (int)a[2]) == 0;
		break;
	case RAVEN_SERVICE_AVAILABLE:
		result = (uint64_t)range_free(a[0], a[1]);
		break;
	case RAVEN_SERVICE_FLUSH: {
		const uint64_t first = a[0] & ~4095ULL, last = (a[0] + a[1] + 4095) & ~4095ULL;
		ok = mprotect((void *)first, last - first, PROT_READ | PROT_WRITE) == 0 && mprotect((void *)first, last - first, (int)a[2]) == 0;
		break;
	}
	case RAVEN_SERVICE_THREAD_CREATE:
		if (a[0] == 0 || a[0] >= RAVEN_GUEST_MAX_THREADS) {
			errno = EINVAL;
			ok = 0;
			break;
		}
		errno = pthread_create(&guest_threads[a[0]], NULL, guest_start, (void *)(uintptr_t)a[0]);
		ok = errno == 0;
		if (ok)
			while (load(&control->threads[a[0]].state) != RAVEN_THREAD_STOPPED)
				wait_change(&control->threads[a[0]].state, RAVEN_THREAD_RUNNING);
		break;
	case RAVEN_SERVICE_THREAD_INTERRUPT:
		if (a[0] >= RAVEN_GUEST_MAX_THREADS) {
			errno = EINVAL;
			ok = 0;
		} else {
			interrupt_thread((uint32_t)a[0]);
		}
		break;
	case RAVEN_SERVICE_THREAD_JOIN:
		if (a[0] == 0 || a[0] >= RAVEN_GUEST_MAX_THREADS) {
			errno = EINVAL;
			ok = 0;
			break;
		}
		interrupt_thread((uint32_t)a[0]);
		while (load(&control->threads[a[0]].state) != RAVEN_THREAD_STOPPED) {
			const uint32_t state = load(&control->threads[a[0]].state);
			if (state != RAVEN_THREAD_STOPPED)
				wait_change(&control->threads[a[0]].state, state);
		}
		store(&control->threads[a[0]].state, RAVEN_THREAD_EXIT);
		errno = pthread_join(guest_threads[a[0]], NULL);
		ok = errno == 0;
		if (ok)
			__atomic_store_n(&raven_unix_threads[a[0]].identity, 0, __ATOMIC_RELEASE);
		break;
	case RAVEN_SERVICE_COMPARE_EXCHANGE:
		if ((a[3] != 4 && a[3] != 8) || a[0] % a[3]) {
			errno = EINVAL;
			ok = 0;
			break;
		}
		result = compare_exchange(a[0], a[1], a[2], a[3]);
		ok = errno == 0;
		break;
	case RAVEN_SERVICE_LOAD_UNIXLIB: {
		if (!a[0] || a[0] > 4096 || unix_library_count == 32 || !memchr(data_area, 0, RAVEN_CONTROL_DATA_SIZE)) {
			errno = EINVAL;
			ok = 0;
			break;
		}
		void *module = dlopen((const char *)data_area, RTLD_NOW | RTLD_LOCAL);
		void *functions = module ? dlsym(module, "__wine_unix_call_funcs") : NULL;
		if (!functions) {
			const char *error = dlerror();
			fprintf(stderr, "raven-bootstrap: unixlib: %s\n", error ? error : "missing function table");
			if (module)
				dlclose(module);
			errno = ENOEXEC;
			ok = 0;
			break;
		}
		const uint32_t index = unix_library_count;
		void (*initialize)(struct raven_control *) = dlsym(module, "raven_unixlib_init");
		if (initialize)
			initialize(control);
		void *(*wrap)(void *, uint32_t) = dlsym(module, "raven_unixlib_wrap_table");
		if (wrap)
			functions = wrap(functions, (uint32_t)a[0]);
		if (!functions) {
			errno = ENOEXEC;
			ok = 0;
			dlclose(module);
			break;
		}
		unix_libraries[index] = (struct unix_library){module, functions, (uint32_t)a[0]};
		__atomic_store_n(&unix_library_count, index + 1, __ATOMIC_RELEASE);
		result = (uint64_t)functions;
		break;
	}
	case RAVEN_SERVICE_EXIT:
		_exit(0);
	default:
		errno = ENOSYS;
		ok = 0;
	}
	service->result = result;
	service->error = ok ? 0 : (errno ? errno : EINVAL);
}

static void *service_loop(void *unused)
{
	(void)unused;
	struct raven_service_channel *service = &control->service;
	// The host can post its first request before this pthread gets the processor. The initial sequence belongs to the zeroed block.
	uint32_t seen = 0;
	for (;;) {
		const uint32_t request = wait_change(&service->request, seen);
		serve(service);
		seen = request;
		store(&service->reply, request);
	}
	return NULL;
}

// The guest has no reason to outlive the host that drives it.
static void *watch_host(void *unused)
{
	(void)unused;
	const int queue = kqueue();
	struct kevent change;
	EV_SET(&change, getppid(), EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
	struct kevent event;
	if (queue < 0 || kevent(queue, &change, 1, &event, 1, NULL) >= 0 || getppid() == 1)
		_exit(1);
	return NULL;
}

// ---- the guest thread's stops

static uint64_t linux_cs(uint64_t cs)
{
	if (cs == RAVEN_MAC_CS64)
		return 0x33;
	if (cs == RAVEN_LDT_SELECTOR(RAVEN_LDT_CODE32))
		return 0x23;
	return cs;
}

static uint64_t mac_cs(uint64_t cs)
{
	if (cs == 0x23)
		return RAVEN_LDT_SELECTOR(RAVEN_LDT_CODE32);
	return RAVEN_MAC_CS64;
}

// The runtime numbers TEB descriptors from 0, as Linux lets it; here they start at RAVEN_LDT_FIRST_TEB.
static uint64_t linux_fs(uint64_t fs)
{
	return fs >= RAVEN_LDT_SELECTOR(RAVEN_LDT_FIRST_TEB) ? fs - RAVEN_LDT_FIRST_TEB * 8 : 0;
}

static uint64_t mac_fs(uint64_t fs)
{
	return fs ? fs + RAVEN_LDT_FIRST_TEB * 8 : 0;
}

// Where a signal context keeps each part of the state. Its layout depends on the processor state macOS saves (with or without AVX or
// AVX-512) and on whether the thread has set its GS base, which makes the kernel save the "full" thread state, with GS base included.
struct context_layout {
	_STRUCT_X86_THREAD_STATE64 *thread;
	uint8_t *legacy;
	uint8_t *ymm_upper;
	uint64_t *gs_base;
};

#define LAYOUT(type, full, avx)                                                                                                          \
	if (context->uc_mcsize == sizeof(type)) {                                                                                             \
		type *m = (type *)context->uc_mcontext;                                                                                           \
		struct context_layout layout = {0};                                                                                               \
		layout.thread = full;                                                                                                             \
		layout.legacy = (uint8_t *)&m->__fs + offsetof(_STRUCT_X86_FLOAT_STATE64, __fpu_fcw);                                             \
		layout.ymm_upper = avx;                                                                                                           \
		return layout;                                                                                                                    \
	}

static struct context_layout layout_of(ucontext_t *context)
{
	LAYOUT(_STRUCT_MCONTEXT64, &m->__ss, NULL)
	LAYOUT(_STRUCT_MCONTEXT_AVX64, &m->__ss, (uint8_t *)&m->__fs.__fpu_ymmh0)
	LAYOUT(_STRUCT_MCONTEXT_AVX512_64, &m->__ss, (uint8_t *)&m->__fs.__fpu_ymmh0)
	struct context_layout full = {0};
#define FULL_LAYOUT(type, avx)                                                                                                           \
	if (context->uc_mcsize == sizeof(type)) {                                                                                             \
		type *m = (type *)context->uc_mcontext;                                                                                           \
		full.thread = &m->__ss.__ss64;                                                                                                    \
		full.legacy = (uint8_t *)&m->__fs + offsetof(_STRUCT_X86_FLOAT_STATE64, __fpu_fcw);                                              \
		full.ymm_upper = avx;                                                                                                             \
		full.gs_base = &m->__ss.__gsbase;                                                                                                 \
		return full;                                                                                                                      \
	}
	FULL_LAYOUT(_STRUCT_MCONTEXT64_FULL, NULL)
	FULL_LAYOUT(_STRUCT_MCONTEXT_AVX64_FULL, (uint8_t *)&m->__fs.__fpu_ymmh0)
	FULL_LAYOUT(_STRUCT_MCONTEXT_AVX512_64_FULL, (uint8_t *)&m->__fs.__fpu_ymmh0)
	fprintf(stderr, "raven-bootstrap: unknown signal context layout of %u bytes\n", (unsigned)context->uc_mcsize);
	_exit(123);
}

static void save_context(ucontext_t *context, struct raven_thread_channel *thread, uint32_t index, int signal)
{
	const struct context_layout layout = layout_of(context);
	const _STRUCT_X86_THREAD_STATE64 *s = layout.thread;
	struct raven_registers *r = &thread->registers;
	r->rax = s->__rax; r->rbx = s->__rbx; r->rcx = s->__rcx; r->rdx = s->__rdx;
	r->rdi = s->__rdi; r->rsi = s->__rsi; r->rbp = s->__rbp; r->rsp = s->__rsp;
	r->r8 = s->__r8; r->r9 = s->__r9; r->r10 = s->__r10; r->r11 = s->__r11;
	r->r12 = s->__r12; r->r13 = s->__r13; r->r14 = s->__r14; r->r15 = s->__r15;
	r->rip = s->__rip;
	r->eflags = s->__rflags & ~0x100ULL;
	r->cs = linux_cs(s->__cs);
	const int compat = r->cs == 0x23;
	r->ss = 0x2b;
	r->ds = r->es = compat ? 0x2b : 0;
	r->fs = linux_fs(s->__fs);
	const uint64_t fs_index = s->__fs >> 3;
	r->fs_base = compat && fs_index < LDT_ENTRIES ? ldt_bases[fs_index] : 0;
	r->gs = 0;
	r->gs_base = compat ? 0 : interrupted_gs_base(r->rip, index);
	r->orig_rax = signal == SIGSYS ? s->__rax : (uint64_t)-1;
	memcpy(thread->xstate, layout.legacy, 512);
	memset(thread->xstate + RAVEN_XSTATE_HEADER, 0, 64);
	uint64_t components = 3;
	if (layout.ymm_upper) {
		memcpy(thread->xstate + RAVEN_XSTATE_YMM, layout.ymm_upper, 256);
		components |= 4;
	} else {
		memset(thread->xstate + RAVEN_XSTATE_YMM, 0, 256);
	}
	memcpy(thread->xstate + RAVEN_XSTATE_HEADER, &components, sizeof components);
}

static void load_context(ucontext_t *context, const struct raven_thread_channel *thread, uint32_t index)
{
	const struct context_layout layout = layout_of(context);
	_STRUCT_X86_THREAD_STATE64 *s = layout.thread;
	const struct raven_registers *r = &thread->registers;
	s->__rax = r->rax; s->__rbx = r->rbx; s->__rcx = r->rcx; s->__rdx = r->rdx;
	s->__rdi = r->rdi; s->__rsi = r->rsi; s->__rbp = r->rbp; s->__rsp = r->rsp;
	s->__r8 = r->r8; s->__r9 = r->r9; s->__r10 = r->r10; s->__r11 = r->r11;
	s->__r12 = r->r12; s->__r13 = r->r13; s->__r14 = r->r14; s->__r15 = r->r15;
	s->__rip = r->rip;
	s->__rflags = (r->eflags & 0x0cd5) | 0x202 | (thread->trace ? 0x100 : 0);
	s->__cs = mac_cs(r->cs);
	const int compat = r->cs == 0x23;
	s->__fs = compat ? mac_fs(r->fs) : 0;
	s->__gs = 0;
	const uint64_t restore_gs = compat ? raven_agent_tsds[index] : r->gs_base;
	if (!inside_unix_call(r->rip, index))
		guest_gs_bases[index] = restore_gs;
	// Returning from the handler restores the GS base of a full context: it must be the guest's, not the one it stopped with.
	if (layout.gs_base)
		*layout.gs_base = restore_gs;
	// MXCSR_MASK is the processor's, not the program's.
	uint32_t mask;
	memcpy(&mask, layout.legacy + 28, 4);
	memcpy(layout.legacy, thread->xstate, 512);
	memcpy(layout.legacy + 28, &mask, 4);
	uint64_t components;
	memcpy(&components, thread->xstate + RAVEN_XSTATE_HEADER, sizeof components);
	if (layout.ymm_upper) {
		if (components & 4)
			memcpy(layout.ymm_upper, thread->xstate + RAVEN_XSTATE_YMM, 256);
		else
			memset(layout.ymm_upper, 0, 256);
	}
}

void raven_agent_stop(int signal, siginfo_t *info, void *raw, uint32_t index)
{
	if (index >= RAVEN_GUEST_MAX_THREADS) {
		// Native dispatch/Metal workers have no Windows channel or alternate stack. A stray scheduler signal has no work for them.
		if (signal == SIGUSR1)
			return;
		// A fault of a guarded copy the service makes, not of the guest.
		if (pthread_equal(pthread_self(), service_thread) && service_guarded)
			siglongjmp(service_recovery, 1);
		fprintf(stderr, "raven-bootstrap: service fault %d at %p\n", signal, info->si_addr);
		_exit(124);
	}
	// Scheduler handshakes can set errno while interrupting a native library on this pthread.
	const int saved_errno = errno;
	ucontext_t *context = raw;
	struct raven_thread_channel *thread = &control->threads[index];
	const uint32_t requested = load(&thread->interrupts_requested);
	if (signal == SIGUSR1 && load(&thread->interrupts_answered) == requested) {
		// An interrupt asked for while the guest was already stopping: that stop answered it.
		const struct context_layout layout = layout_of(context);
		const uint64_t base = interrupted_gs_base(layout.thread->__rip, index);
		if (layout.gs_base)
			*layout.gs_base = base;
		errno = saved_errno;
		set_gs_base(base);
		return;
	}
	save_context(context, thread, index, signal);
	if (inside_unix_call(thread->registers.rip, index) && (signal == SIGSEGV || signal == SIGBUS || signal == SIGILL)) {
		Dl_info image = {0};
		dladdr((void *)thread->registers.rip, &image);
		fprintf(stderr, "raven-bootstrap: native fault %d in %s+0x%llx (%s) at %p\n", signal,
			image.dli_fname ? image.dli_fname : "unknown", thread->registers.rip - (uint64_t)image.dli_fbase,
			image.dli_sname ? image.dli_sname : "unknown", info->si_addr);
		void *frames[32];
		backtrace_symbols_fd(frames, backtrace(frames, 32), STDERR_FILENO);
	}
	thread->signal = signal == SIGUSR1 ? RAVEN_STOP_INTERRUPT : signal;
	thread->code = info->si_code;
	thread->fault_address = (uint64_t)info->si_addr;
	thread->interrupts_answered = requested;
	store(&thread->state, RAVEN_THREAD_STOPPED);
	// The state is authoritative. A full nonblocking pipe already guarantees a scheduler wake.
	if (load(&control->parallel)) {
		const char byte = 1;
		(void)write(RAVEN_GUEST_WAKE_FD, &byte, 1);
	}
	const uint32_t next = wait_change(&thread->state, RAVEN_THREAD_STOPPED);
	if (next == RAVEN_THREAD_EXIT)
		pthread_exit(NULL);
	load_context(context, thread, index);
	__atomic_store_n(&thread->state, RAVEN_THREAD_RUNNING, __ATOMIC_RELEASE);
	errno = saved_errno;
	set_gs_base(thread->registers.cs == 0x23 ? raven_agent_tsds[index] : thread->registers.gs_base);
}

// The first instructions of every handler: on the guest thread, give macOS code its thread data back before anything else runs.
__attribute__((naked)) static void stop_entry(int signal, siginfo_t *info, void *context)
{
	__asm__ volatile(
		"leaq _raven_agent_altstacks(%%rip), %%rax\n"
		"movq %%rsp, %%r8\n"
		"subq %%rax, %%r8\n"
		"shrq $18, %%r8\n"
		"movl %[count], %%ecx\n"
		"cmpq %[count], %%r8\n"
		"jae 1f\n"
		"pushq %%rdi\n"
		"pushq %%rsi\n"
		"pushq %%rdx\n"
		"leaq _raven_agent_tsds(%%rip), %%rax\n"
		"movq (%%rax, %%r8, 8), %%rdi\n"
		"movl $0x3000003, %%eax\n"
		"syscall\n"
		"popq %%rdx\n"
		"popq %%rsi\n"
		"popq %%rdi\n"
		"movl %%r8d, %%ecx\n"
		"1:\n"
		"jmp _raven_agent_stop\n"
		: : [count] "i"(RAVEN_GUEST_MAX_THREADS));
}

static void prepare_guest(uint32_t index)
{
	raven_agent_tsds[index] = find_tsd_base();
	guest_gs_bases[index] = raven_agent_tsds[index];
	// Reuse the guarded stack when the channel is joined and assigned to a new pthread.
	struct unix_thread *native = &raven_unix_threads[index];
	if (!native->stack_top) {
		const size_t size = 2 * 1024 * 1024;
		void *stack = mmap(NULL, size + 8192, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (stack == MAP_FAILED || mprotect((char *)stack + 4096, size, PROT_READ | PROT_WRITE))
			fail("cannot create the unix-call stack");
		native->stack_top = (char *)stack + 4096 + size;
	}
	native->tsd = raven_agent_tsds[index];
	native->guest_gs = &guest_gs_bases[index];
	native->active = &control->threads[index].unix_active;
	uint64_t identity;
	pthread_threadid_np(NULL, &identity);
	__atomic_store_n(&native->identity, identity, __ATOMIC_RELEASE);
	stack_t alternate = {.ss_sp = raven_agent_altstacks[index], .ss_size = ALTSTACK_SIZE};
	if (sigaltstack(&alternate, NULL) != 0)
		fail("cannot set the signal stack");
}

static void *guest_start(void *raw)
{
	const uint32_t index = (uint32_t)(uintptr_t)raw;
	prepare_guest(index);
	sigset_t interrupt;
	sigemptyset(&interrupt);
	sigaddset(&interrupt, SIGUSR1);
	pthread_sigmask(SIG_UNBLOCK, &interrupt, NULL);
	__asm__ volatile("int3" ::: "memory");
	return NULL;
}

// Where host calls return to: a breakpoint the agent owns.
__attribute__((naked, aligned(16))) static void return_gate(void)
{
	__asm__ volatile("int3\n" "int3\n" "ud2\n");
}

static void send_task_port(const char *name)
{
	mach_port_t host;
	if (bootstrap_look_up(bootstrap_port, name, &host) != KERN_SUCCESS) {
		errno = ENOENT;
		fail("cannot find the host");
	}
	struct {
		mach_msg_header_t header;
		mach_msg_body_t body;
		mach_msg_port_descriptor_t task;
	} message = {0};
	message.header.msgh_bits = MACH_MSGH_BITS_SET(MACH_MSG_TYPE_COPY_SEND, 0, 0, MACH_MSGH_BITS_COMPLEX);
	message.header.msgh_size = sizeof message;
	message.header.msgh_remote_port = host;
	message.body.msgh_descriptor_count = 1;
	message.task.name = mach_task_self();
	message.task.disposition = MACH_MSG_TYPE_COPY_SEND;
	message.task.type = MACH_MSG_PORT_DESCRIPTOR;
	if (mach_msg(&message.header, MACH_SEND_MSG, sizeof message, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) != KERN_SUCCESS) {
		errno = EPROTO;
		fail("cannot send the task port");
	}
	mach_port_deallocate(mach_task_self(), host);
}

int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	void *mapped = mmap(NULL, RAVEN_CONTROL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, RAVEN_GUEST_CONTROL_FD, 0);
	if (mapped == MAP_FAILED)
		fail("cannot map the control block");
	control = mapped;
	data_area = (uint8_t *)mapped + RAVEN_CONTROL_DATA_OFFSET;
	if (control->magic != RAVEN_GUEST_MAGIC || control->version != RAVEN_GUEST_VERSION) {
		errno = EPROTO;
		fail("control block version differs");
	}
	fcntl(RAVEN_GUEST_CONTROL_FD, F_SETFD, FD_CLOEXEC);
	fcntl(RAVEN_GUEST_SOCKET_FD, F_SETFD, FD_CLOEXEC);
	fcntl(RAVEN_GUEST_WAKE_FD, F_SETFD, FD_CLOEXEC);
	// Nothing may be found at an address no Windows allocation made: the reserve becomes no-access memory.
	if (mprotect((void *)RAVEN_LOW_START, RAVEN_LOW_END - RAVEN_LOW_START, PROT_NONE) != 0)
		fail("cannot close the low reserve");
	if (set_ldt(RAVEN_LDT_CODE32, 0, 0xffffffffULL, 1) != 0)
		fail("cannot install the 32-bit code segment");
	guest_threads[0] = pthread_self();
	prepare_guest(0);

	struct sigaction action = {0};
	action.sa_sigaction = stop_entry;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigfillset(&action.sa_mask);
	const int stops[] = {SIGTRAP, SIGILL, SIGSEGV, SIGBUS, SIGFPE, SIGSYS, SIGUSR1};
	for (size_t index = 0; index < sizeof stops / sizeof *stops; index++)
		if (sigaction(stops[index], &action, NULL) != 0)
			fail("cannot install the stop handler");
	signal(SIGPIPE, SIG_IGN);

	// Interrupts from the host are for the guest thread alone.
	sigset_t interrupt, previous;
	sigemptyset(&interrupt);
	sigaddset(&interrupt, SIGUSR1);
	pthread_sigmask(SIG_BLOCK, &interrupt, &previous);
	pthread_t watcher;
	if (pthread_create(&service_thread, NULL, service_loop, NULL) != 0 || pthread_create(&watcher, NULL, watch_host, NULL) != 0)
		fail("cannot start the agent threads");
	pthread_sigmask(SIG_SETMASK, &previous, NULL);

	control->return_gate = (uint64_t)return_gate;
	control->agent_pid = (uint64_t)getpid();
	control->unix_call_gate = (uint64_t)raven_unix_call_gate;
	control->unix_call_gate_end = (uint64_t)raven_unix_call_gate_end;
	send_task_port(argv[1]);
	__atomic_store_n(&control->ready, 1, __ATOMIC_RELEASE);
	// The setup stop. Every context after it is the host's.
	__asm__ volatile("int3" ::: "memory");
	_exit(0);
}
