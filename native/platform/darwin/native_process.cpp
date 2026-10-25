// macOS implementation of NativeProcess: the guest is an x86_64 Mach-O agent (guest/agent.c) that runs under Rosetta 2.
//
// Linux drives a ptraced ELF child from outside (cpu/native_cpu.cpp). Rosetta exposes neither ptrace nor the x86 register state of a
// translated thread to another task, so here the agent catches its own stops with signal handlers and trades the registers through a
// shared control block (guest/protocol.h); a thread of the agent maps and protects memory for the host. The host reads and writes guest
// memory through the agent's task port, and through the agent itself where the kernel will not let it.
//
// This follows the Linux controller call for call: the stops, the stack frames and the bookkeeping of mappings are the same, so the
// runtime above sees one behaviour. By default Windows threads share the main agent thread; RAVEN_PARALLEL gives each its own pthread.

#include "cpu/native_cpu.h"
#include "cpu/served_queue.h"
#include "guest/protocol.h"
#include "controller_profile.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include <execinfo.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <os/os_sync_wait_on_address.h>
#include <servers/bootstrap.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace raven {

extern const char *api_in_progress;

namespace {

uint32_t load(const uint32_t *word)
{
	return __atomic_load_n(word, __ATOMIC_ACQUIRE);
}

void store(uint32_t *word, uint32_t value)
{
	__atomic_store_n(word, value, __ATOMIC_RELEASE);
	os_sync_wake_by_address_all(word, sizeof *word, OS_SYNC_WAKE_BY_ADDRESS_SHARED);
}

void sleep_on(uint32_t *word, uint32_t value, std::chrono::nanoseconds limit)
{
	const auto nanoseconds = std::max<int64_t>(1000, std::min<int64_t>(limit.count(), 10'000'000));
	os_sync_wait_on_address_with_timeout(word, value, sizeof *word, OS_SYNC_WAIT_ON_ADDRESS_SHARED, OS_CLOCK_MACH_ABSOLUTE_TIME,
		uint64_t(nanoseconds));
}

std::chrono::microseconds controller_spin()
{
	static const auto spin = [] {
		const char *value = std::getenv("RAVEN_DARWIN_SPIN_US");
		return std::chrono::microseconds(value ? std::clamp<long>(std::strtol(value, nullptr, 10), 0, 1000) : 60);
	}();
	return spin;
}

void backtrace_if_asked()
{
	if (std::getenv("RAVEN_BACKTRACE")) {
		void *frames[40];
		backtrace_symbols_fd(frames, backtrace(frames, 40), 2);
	}
}

// Spawn actions can overwrite descriptors 3 through 5, and Darwin's spawn does not clear CLOEXEC for a dup2 onto itself.
int spawn_source(int descriptor)
{
	if (descriptor < 0 || descriptor > RAVEN_GUEST_WAKE_FD)
		return descriptor;
	const int moved = fcntl(descriptor, F_DUPFD_CLOEXEC, RAVEN_GUEST_WAKE_FD + 1);
	const int error = errno;
	close(descriptor);
	errno = error;
	return moved;
}

}

// The agent process and the channels to it.
class GuestAgent {
public:
	GuestAgent(const std::string &bootstrap, std::chrono::milliseconds timeout);
	~GuestAgent();
	GuestAgent(const GuestAgent &) = delete;
	GuestAgent &operator=(const GuestAgent &) = delete;

	pid_t pid = -1;
	mach_port_t task = MACH_PORT_NULL;
	raven_control *control = nullptr;
	uint8_t *data = nullptr;
	uint64_t gate = 0;
	mutable ControllerProfile profile;

	pid_t selected = 1;
	bool parallel = false;
	raven_thread_channel &thread() { return control->threads[slots.at(selected)]; }
	raven_registers &registers() { return thread().registers; }
	int execution_descriptor() const { return parallel ? execution_wake : -1; }
	void drain_execution();
	pid_t new_thread();
	void stop_thread(pid_t thread);
	void pause_thread();
	void select(pid_t thread)
	{
		if (!slots.contains(thread))
			throw std::runtime_error("native thread has no execution task");
		selected = thread;
	}
	bool alive();
	void stop();
	void set_timeout(std::chrono::milliseconds timeout);
	// 1 once the guest thread has stopped, -1 at the deadline, -2 at the end of a slice. `served` is polled while the guest runs.
	int wait(const std::function<bool()> *served = nullptr);
	void resume(bool trace);
	void interrupt();
	uint64_t service(uint32_t command, std::initializer_list<uint64_t> arguments, const char *what);
	void send_descriptor(int descriptor);
	void set_slice(std::chrono::steady_clock::duration slice) { slice_end = std::chrono::steady_clock::now() + slice; }
	void ensure_slice(std::chrono::steady_clock::time_point end)
	{
		if (!slice_end)
			slice_end = end;
	}
	void clear_slice() { slice_end.reset(); }

private:
	int control_descriptor = -1, host_socket = -1, execution_wake = -1;
	std::map<pid_t, uint32_t> slots{{1, 0}};
	pid_t next_thread = 2;
	std::chrono::steady_clock::time_point deadline;
	std::optional<std::chrono::steady_clock::time_point> slice_end;
	void reap(bool block);
};

GuestAgent::GuestAgent(const std::string &bootstrap, std::chrono::milliseconds timeout)
	: deadline(std::chrono::steady_clock::now() + timeout)
{
	try {
		control_descriptor = spawn_source(memfd_create("raven-guest-control", MFD_CLOEXEC));
		if (control_descriptor < 0 || ftruncate(control_descriptor, RAVEN_CONTROL_SIZE) != 0)
			throw std::runtime_error("cannot create the guest control block");
		void *mapped = mmap(nullptr, RAVEN_CONTROL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, control_descriptor, 0);
		if (mapped == MAP_FAILED)
			throw std::runtime_error("cannot map the guest control block");
		control = static_cast<raven_control *>(mapped);
		data = static_cast<uint8_t *>(mapped) + RAVEN_CONTROL_DATA_OFFSET;
		control->magic = RAVEN_GUEST_MAGIC;
		control->version = RAVEN_GUEST_VERSION;
		int pair[2];
		if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0)
			throw std::runtime_error("cannot create the guest descriptor channel");
		host_socket = pair[0];
		const int guest_socket = spawn_source(pair[1]);
		if (guest_socket < 0)
			throw std::runtime_error("cannot reserve the guest descriptor channel");
		int wake[2];
		if (pipe(wake) != 0) {
			close(guest_socket);
			throw std::runtime_error("cannot create the guest stop channel");
		}
		execution_wake = wake[0];
		wake[1] = spawn_source(wake[1]);
		if (wake[1] < 0) {
			close(guest_socket);
			throw std::runtime_error("cannot reserve the guest stop channel");
		}
		fcntl(wake[0], F_SETFD, FD_CLOEXEC);
		fcntl(wake[1], F_SETFD, FD_CLOEXEC);
		fcntl(wake[0], F_SETFL, O_NONBLOCK);
		fcntl(wake[1], F_SETFL, O_NONBLOCK);
		struct WakeGuard {
			int descriptor;
			~WakeGuard() { close(descriptor); }
		} wake_guard{wake[1]};

		// The agent hands over its task port through a name only this host knows.
		mach_port_t port = MACH_PORT_NULL;
		static std::atomic<unsigned> agents{0};
		const auto name = "org.raven.guest." + std::to_string(getpid()) + "." + std::to_string(agents.fetch_add(1)) + "." +
			std::to_string(arc4random());
		if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) != KERN_SUCCESS ||
			mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS) {
			close(guest_socket);
			throw std::runtime_error("cannot create the guest task port channel");
		}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		const kern_return_t registered = bootstrap_register(bootstrap_port, const_cast<char *>(name.c_str()), port);
#pragma clang diagnostic pop
		struct PortGuard {
			mach_port_t port;
			~PortGuard() { mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1); mach_port_deallocate(mach_task_self(), port); }
		} guard{port};
		if (registered != KERN_SUCCESS) {
			close(guest_socket);
			throw std::runtime_error("cannot publish the guest task port channel");
		}

		posix_spawn_file_actions_t actions;
		posix_spawn_file_actions_init(&actions);
		// Darwin's spawn leaves CLOEXEC set for dup2(fd, fd). A host driver can leave the low descriptors free.
		if (control_descriptor == RAVEN_GUEST_CONTROL_FD)
			posix_spawn_file_actions_addinherit_np(&actions, control_descriptor);
		else
			posix_spawn_file_actions_adddup2(&actions, control_descriptor, RAVEN_GUEST_CONTROL_FD);
		if (guest_socket == RAVEN_GUEST_SOCKET_FD)
			posix_spawn_file_actions_addinherit_np(&actions, guest_socket);
		else
			posix_spawn_file_actions_adddup2(&actions, guest_socket, RAVEN_GUEST_SOCKET_FD);
		posix_spawn_file_actions_adddup2(&actions, wake[1], RAVEN_GUEST_WAKE_FD);
		posix_spawn_file_actions_addinherit_np(&actions, STDERR_FILENO);
		posix_spawnattr_t attributes;
		posix_spawnattr_init(&attributes);
		posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF |
			POSIX_SPAWN_SETSIGMASK);
		posix_spawnattr_setpgroup(&attributes, 0);
		sigset_t none, all;
		sigemptyset(&none);
		sigfillset(&all);
		posix_spawnattr_setsigmask(&attributes, &none);
		posix_spawnattr_setsigdefault(&attributes, &all);
		// The x86_64 agent runs under Rosetta 2, which offers AVX to the guest only when asked.
		std::string path = bootstrap;
		char *arguments[] = {path.data(), const_cast<char *>(name.c_str()), nullptr};
		char avx[] = "ROSETTA_ADVERTISE_AVX=1";
		char *environment[] = {avx, nullptr};
		const int spawned = posix_spawn(&pid, path.c_str(), &actions, &attributes, arguments, environment);
		posix_spawn_file_actions_destroy(&actions);
		posix_spawnattr_destroy(&attributes);
		close(guest_socket);
		if (spawned != 0) {
			pid = -1;
			throw std::runtime_error("cannot start the guest agent " + bootstrap + ": " + std::strerror(spawned));
		}

		struct {
			mach_msg_header_t header;
			mach_msg_body_t body;
			mach_msg_port_descriptor_t task;
			mach_msg_trailer_t trailer;
		} message{};
		for (;;) {
			const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
			if (left <= 0)
				throw std::runtime_error("the guest agent did not report");
			const auto received = mach_msg(&message.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof message, port,
				mach_msg_timeout_t(std::min<long long>(left, 100)), MACH_PORT_NULL);
			if (received == KERN_SUCCESS)
				break;
			if (received != MACH_RCV_TIMED_OUT)
				throw std::runtime_error("cannot receive the guest task port");
			if (!alive())
				throw std::runtime_error("the guest agent ended during startup");
		}
		if (!(message.header.msgh_bits & MACH_MSGH_BITS_COMPLEX) || message.body.msgh_descriptor_count != 1)
			throw std::runtime_error("invalid guest task port message");
		task = message.task.name;
		int task_pid = -1;
		if (pid_for_task(task, &task_pid) != KERN_SUCCESS || task_pid != pid)
			throw std::runtime_error("the guest task port belongs to another process");
		const int stopped = wait();
		if (stopped != 1 || thread().signal != SIGTRAP)
			throw std::runtime_error("the guest agent did not reach its setup stop (wait " + std::to_string(stopped) + ", signal " +
				std::to_string(thread().signal) + ", state " + std::to_string(thread().state) + ")");
		gate = control->return_gate;
	} catch (...) {
		stop();
		throw;
	}
}

GuestAgent::~GuestAgent()
{
	stop();
	profile.report();
}

void GuestAgent::reap(bool block)
{
	if (pid <= 0)
		return;
	int status = 0;
	pid_t result;
	do {
		result = waitpid(pid, &status, block ? 0 : WNOHANG);
	} while (result == -1 && errno == EINTR);
	if (result == pid || (result == -1 && errno == ECHILD)) {
		if (std::getenv("RAVEN_TRACE_API") && result == pid)
			std::cerr << "raven: guest agent " << (WIFSIGNALED(status) ? "killed by signal " + std::to_string(WTERMSIG(status)) :
				"exited with " + std::to_string(WEXITSTATUS(status))) << '\n';
		pid = -1;
	}
}

bool GuestAgent::alive()
{
	reap(false);
	return pid > 0;
}

void GuestAgent::stop()
{
	if (pid > 0) {
		kill(pid, SIGKILL);
		reap(true);
	}
	if (task != MACH_PORT_NULL) {
		mach_port_deallocate(mach_task_self(), task);
		task = MACH_PORT_NULL;
	}
	if (execution_wake >= 0) {
		close(execution_wake);
		execution_wake = -1;
	}
	if (host_socket >= 0) {
		close(host_socket);
		host_socket = -1;
	}
	if (control) {
		munmap(control, RAVEN_CONTROL_SIZE);
		control = nullptr;
		data = nullptr;
	}
	if (control_descriptor >= 0) {
		close(control_descriptor);
		control_descriptor = -1;
	}
}

void GuestAgent::set_timeout(std::chrono::milliseconds timeout)
{
	if (timeout.count() <= 0)
		throw std::runtime_error("native execution requires a positive timeout");
	deadline = std::chrono::steady_clock::now() + timeout;
}

int GuestAgent::wait(const std::function<bool()> *served)
{
	ControllerProfile::Scope timing(profile, profile.waits);
	auto *state = &thread().state;
	// Guest code usually reaches its next stop within microseconds, so it is polled first; a guest that runs longer is waited for asleep.
	unsigned polls = 0;
	auto spin_end = std::chrono::steady_clock::now() + controller_spin();
	for (;;) {
		const auto observed = load(state);
		if (observed == RAVEN_THREAD_STOPPED)
			return 1;
		if (served && (*served)()) {
			polls = 0;
			spin_end = std::chrono::steady_clock::now() + controller_spin();
			if (slice_end && std::chrono::steady_clock::now() >= *slice_end)
				return -2;
			continue;
		}
		if (++polls % 64)
			continue;
		const auto now = std::chrono::steady_clock::now();
		if (slice_end && now >= *slice_end)
			return -2;
		if (now >= deadline)
			return -1;
		if (now < spin_end)
			continue;
		if (!alive())
			throw std::runtime_error("native guest exited outside the runtime boundary");
		const auto until = slice_end ? std::min(*slice_end, deadline) : deadline;
		// A served call ends in a breakpoint if the runtime is not looking, which wakes this sleep.
		if (profile.enabled)
			++profile.wait_sleeps;
		sleep_on(state, observed, std::chrono::duration_cast<std::chrono::nanoseconds>(until - now));
	}
}

void GuestAgent::resume(bool trace)
{
	thread().trace = trace;
	store(&thread().state, RAVEN_THREAD_RESUME);
}

void GuestAgent::interrupt()
{
	__atomic_fetch_add(&thread().interrupts_requested, 1, __ATOMIC_ACQ_REL);
	service(RAVEN_SERVICE_THREAD_INTERRUPT, {slots.at(selected)}, "thread interrupt");
}

void GuestAgent::drain_execution()
{
	char bytes[256];
	while (read(execution_wake, bytes, sizeof bytes) > 0) {}
}

pid_t GuestAgent::new_thread()
{
	uint32_t index = 1;
	for (; index < RAVEN_GUEST_MAX_THREADS; ++index)
		if (std::none_of(slots.begin(), slots.end(), [index](const auto &item) { return item.second == index; }))
			break;
	if (index == RAVEN_GUEST_MAX_THREADS || next_thread == INT32_MAX)
		throw std::runtime_error("native guest thread limit reached");
	std::memset(&control->threads[index], 0, sizeof control->threads[index]);
	service(RAVEN_SERVICE_THREAD_CREATE, {index}, "thread creation");
	const pid_t task = next_thread++;
	slots.emplace(task, index);
	return task;
}

void GuestAgent::pause_thread()
{
	if (load(&thread().state) == RAVEN_THREAD_STOPPED)
		return;
	interrupt();
	set_timeout(std::chrono::seconds(2));
	if (wait() != 1)
		throw std::runtime_error("native guest did not stop for suspension");
}

void GuestAgent::stop_thread(pid_t task)
{
	const auto found = slots.find(task);
	if (found == slots.end())
		return;
	if (!found->second)
		throw std::runtime_error("cannot join the main guest thread");
	auto &channel = control->threads[found->second];
	__atomic_fetch_add(&channel.interrupts_requested, 1, __ATOMIC_ACQ_REL);
	service(RAVEN_SERVICE_THREAD_JOIN, {found->second}, "thread exit");
	slots.erase(found);
}

uint64_t GuestAgent::service(uint32_t command, std::initializer_list<uint64_t> arguments, const char *what)
{
	ControllerProfile::Scope timing(profile, profile.services.at(command));
	if (!control || pid <= 0)
		throw std::runtime_error("native guest process is gone");
	auto &channel = control->service;
	channel.command = command;
	channel.error = 0;
	std::fill(std::begin(channel.arguments), std::end(channel.arguments), 0);
	std::copy(arguments.begin(), arguments.end(), channel.arguments);
	const uint32_t request = channel.request + 1;
	store(&channel.request, request);
	const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	const auto spin_end = std::chrono::steady_clock::now() + controller_spin();
	for (unsigned spin = 0;; ++spin) {
		const auto observed = load(&channel.reply);
		if (observed == request)
			break;
		if (spin % 64 || std::chrono::steady_clock::now() < spin_end)
			continue;
		if (!alive())
			throw std::runtime_error(std::string("native guest exited during ") + what);
		if (std::chrono::steady_clock::now() >= limit) {
			stop();
			throw std::runtime_error(std::string("native guest did not answer ") + what);
		}
		if (profile.enabled)
			++profile.service_sleeps;
		// Wait on the value already checked. Reloading here can observe the completed reply and then sleep on it for 10 ms.
		sleep_on(&channel.reply, observed, std::chrono::milliseconds(10));
	}
	if (channel.error)
		throw std::system_error(channel.error, std::generic_category(), std::string("native ") + what);
	return channel.result;
}

void GuestAgent::send_descriptor(int descriptor)
{
	char byte = 1;
	iovec vector{&byte, 1};
	alignas(cmsghdr) char space[CMSG_SPACE(sizeof(int))]{};
	msghdr message{};
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	message.msg_control = space;
	message.msg_controllen = sizeof space;
	auto *header = CMSG_FIRSTHDR(&message);
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SCM_RIGHTS;
	header->cmsg_len = CMSG_LEN(sizeof(int));
	std::memcpy(CMSG_DATA(header), &descriptor, sizeof descriptor);
	ssize_t sent;
	do {
		sent = sendmsg(host_socket, &message, 0);
	} while (sent == -1 && errno == EINTR);
	if (sent != 1)
		throw std::runtime_error("cannot send native mapping descriptor");
}

struct NativeProcess::State {
	std::unique_ptr<GuestAgent> child;
	// The host's own mapping of a view: from the host page at or before its offset, pointed at the guest's first byte.
	struct SharedView {
		void *mapping = MAP_FAILED;
		size_t mapping_size = 0;
		uint8_t *host = nullptr;
		size_t size = 0;
		~SharedView() { if (mapping != MAP_FAILED) munmap(mapping, mapping_size); }
	};
	static std::unique_ptr<SharedView> host_view(int descriptor, size_t size, uint64_t offset = 0)
	{
		auto view = std::make_unique<SharedView>();
		view->size = size;
		const size_t slack = offset % uint64_t(getpagesize());
		if (size > SIZE_MAX - slack)
			throw std::runtime_error("native host view size overflow");
		view->mapping_size = size + slack;
		view->mapping = mmap(nullptr, view->mapping_size, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, off_t(offset - slack));
		if (view->mapping == MAP_FAILED)
			throw std::runtime_error("cannot map native descriptor in host");
		view->host = static_cast<uint8_t *>(view->mapping) + slack;
		return view;
	}
	std::map<uint64_t, std::unique_ptr<SharedView>> shared_views;
	std::map<uint64_t, ImageMapping> mappings;
	std::vector<std::pair<uint64_t, uint64_t>> allocations;
	std::vector<std::pair<uint64_t, uint64_t>> image_allocations;
	std::map<uint64_t, uint32_t> api_traps;
	std::set<uint64_t> returning_traps;
	std::vector<uint8_t> trap_page;
	uint64_t trap_page_address = 0;
	NativeMode mode = NativeMode::x64;
	uint64_t return_gate = 0;
	uint32_t fs_selector = 0;
	uint32_t ldt_entries = 1;
	std::chrono::milliseconds slice{0};
	std::chrono::steady_clock::time_point slice_started = std::chrono::steady_clock::now();
	std::function<bool()> slice_wanted;
	std::function<void()> slice_yield;
	uint64_t slice_interrupts = 0, slice_deferrals = 0, slice_yield_requests = 0;
	uint64_t served_calls = 0, served_waits = 0, served_yields = 0;
	std::optional<std::vector<uint64_t>> translated_words;
	size_t translated_depth = 0;
	std::array<uint64_t, 4> translated_floats{};
	uint64_t translated_result = 0;
	uint64_t fs_base = 0;
	uint32_t pending_cleanup = 0;
	std::vector<NativeContext> callbacks;
	std::vector<uint32_t> callback_cleanups;
	uint64_t stack_top = 0;
	uint64_t initial_stack_top = 0;
	uint64_t initial_stack_size = 1024 * 1024;
	bool stopped = true;
	bool active_call = false;
	bool awaiting_api = false;
	bool awaiting_syscall = false;
	bool debug_print_fault = false;
	size_t steps_left = 0;
	std::function<void(uint64_t)> step_sink;
	uint64_t *served_block = nullptr;
	uint64_t thread_teb = 0;
	uint64_t thread_served_guest = 0;
	uint64_t *thread_served_block = nullptr;
	std::vector<uint64_t> queued_stubs;
	std::map<uint64_t, uint64_t *> queued_blocks;
	std::map<uint64_t *, uint64_t> served_owners;
	uint64_t queued_calls = 0;
	NativeContext initial_context;
	std::vector<uint64_t> served_stubs;
	std::vector<uint64_t> served_yield_traps;
	size_t served_stub_size = 0;
	size_t served_wait_offset = 0;
	std::function<void(uint64_t)> served_handler;
	std::function<void()> resume_hook;
	std::function<bool()> poll_served;
	std::function<void(std::function<bool()>, std::chrono::steady_clock::time_point)> execution_wait;
	bool serving = false, serving_queued = false;
	std::chrono::milliseconds sample_period{0};
	std::function<void(uint64_t)> sampler;
	uint64_t served_stack = 0;
	void require_stopped() const;
	void require_memory() const;
	bool covers(uint64_t address, size_t size, int protection) const;
	int protection_at(uint64_t address) const;
	uint8_t *shared_memory(uint64_t address, size_t &size) const;
	uint64_t limit() const { return mode == NativeMode::x86 ? 0x100000000ULL : 0x800000000000ULL; }
	raven_registers &registers() const { return child->registers(); }
	uint64_t service(uint32_t command, std::initializer_list<uint64_t> arguments, const char *what) const
	{
		return child->service(command, arguments, what);
	}
};

uint8_t *NativeProcess::State::shared_memory(uint64_t address, size_t &size) const
{
	const auto after = shared_views.upper_bound(address);
	if (after != shared_views.begin()) {
		const auto &view = *std::prev(after);
		if (address - view.first < view.second->size) {
			const auto offset = size_t(address - view.first);
			size = std::min(size, view.second->size - offset);
			return view.second->host + offset;
		}
	}
	if (after != shared_views.end())
		size = std::min(size, size_t(after->first - address));
	return nullptr;
}

void NativeProcess::State::require_stopped() const
{
	if (!child || child->pid == -1)
		throw std::runtime_error("native process is not stopped and alive (the guest process is gone)");
	if (!stopped && !serving) {
		backtrace_if_asked();
		throw std::runtime_error("native process is not stopped and alive (it is running or a resume ended without a stop)");
	}
}

void NativeProcess::State::require_memory() const
{
	if (!child || child->pid == -1)
		throw std::runtime_error("native guest process is gone");
	if (!child->parallel)
		require_stopped();
}

bool NativeProcess::State::covers(uint64_t address, size_t size, int protection) const
{
	if (size > UINT64_MAX - address)
		return false;
	const uint64_t end = address + size;
	while (address < end) {
		auto found = mappings.upper_bound(address);
		if (found == mappings.begin())
			return false;
		const auto &mapping = std::prev(found)->second;
		if (address - mapping.address >= mapping.size || (mapping.protection & protection) != protection)
			return false;
		address = std::min(end, mapping.address + mapping.size);
	}
	return true;
}

int NativeProcess::State::protection_at(uint64_t address) const
{
	const auto found = mappings.upper_bound(address);
	if (found == mappings.begin() || address - std::prev(found)->second.address >= std::prev(found)->second.size)
		return PROT_NONE;
	return std::prev(found)->second.protection;
}

NativeProcess::NativeProcess(std::span<LoadedImage *const> images, const std::string &bootstrap,
	std::chrono::milliseconds setup_timeout, NativeMode mode, uint64_t stack_reserve) : state(std::make_unique<State>())
{
	if (setup_timeout.count() <= 0)
		throw std::runtime_error("native execution requires a positive timeout");
	state->mode = mode;
	for (auto *mapped_image : images) {
		if (!mapped_image)
			throw std::runtime_error("null native image mapping");
		if (mapped_image->data().size() > state->limit() || mapped_image->base() > state->limit() - mapped_image->data().size())
			throw std::runtime_error("native image does not fit the guest address space");
	}
	state->child = std::make_unique<GuestAgent>(bootstrap, setup_timeout);
	state->child->parallel = std::getenv("RAVEN_PARALLEL") != nullptr;
	store(&state->child->control->parallel, state->child->parallel);
	for (auto *mapped_image : images) {
		const auto base = mapped_image->base(), size = uint64_t(mapped_image->data().size());
		auto view = State::host_view(mapped_image->descriptor(), size);
		state->child->send_descriptor(mapped_image->descriptor());
		const auto mapped = state->service(RAVEN_SERVICE_MAP_DESCRIPTOR, {base, size, 0, RAVEN_MAP_EXACT, PROT_NONE}, "image mapping");
		if (mapped != base)
			throw std::runtime_error("host does not support exact non-replacing guest mappings");
		for (const auto &mapping : mapped_image->mappings()) {
			state->service(RAVEN_SERVICE_PROTECT, {mapping.address, mapping.size, uint64_t(mapping.protection)}, "image protection");
			state->mappings.insert_or_assign(mapping.address, mapping);
		}
		state->shared_views.emplace(base, std::move(view));
	}
	const uint64_t stack_size = std::clamp<uint64_t>((stack_reserve + 0xffff) & ~uint64_t(0xffff), 1024 * 1024, 64 * 1024 * 1024);
	state->initial_stack_size = stack_size;
	// Stack arguments, return addresses and callback frames are read and written for almost every API. Keep a coherent host alias.
	const int stack_file = memfd_create("raven-stack", MFD_CLOEXEC);
	if (stack_file < 0)
		throw std::runtime_error("cannot create native stack backing");
	uint64_t stack;
	try {
		if (ftruncate(stack_file, off_t(stack_size + 8192)) != 0)
			throw std::runtime_error("cannot size native stack backing");
		auto view = State::host_view(stack_file, stack_size + 8192);
		state->child->send_descriptor(stack_file);
		stack = state->service(RAVEN_SERVICE_MAP_DESCRIPTOR, {0, stack_size + 8192, 0,
			uint64_t(mode == NativeMode::x86 ? RAVEN_MAP_LOW : RAVEN_MAP_GRANULAR), PROT_NONE}, "stack mapping");
		state->shared_views.emplace(stack, std::move(view));
	} catch (...) {
		close(stack_file);
		throw;
	}
	close(stack_file);
	if (stack + stack_size + 8192 > state->limit())
		throw std::runtime_error("host placed the native stack outside the guest address space");
	state->service(RAVEN_SERVICE_PROTECT, {stack + 4096, stack_size, PROT_READ | PROT_WRITE}, "stack protection");
	state->mappings.insert_or_assign(stack + 4096, ImageMapping{stack + 4096, stack_size, 0, PROT_READ | PROT_WRITE});
	state->stack_top = stack + 4096 + stack_size;
	state->initial_stack_top = state->stack_top;
	if (mode == NativeMode::x86) {
		// 32-bit return addresses cannot name the agent's gate, so calls return through an INT3 page below 4 GiB.
		const auto page = state->service(RAVEN_SERVICE_MAP, {0, 4096, PROT_READ | PROT_WRITE, RAVEN_MAP_LOW}, "return gate");
		state->mappings.insert_or_assign(page, ImageMapping{page, 4096, 0, PROT_READ | PROT_WRITE});
		write_memory(page, std::vector<uint8_t>{0xcc});
		state->service(RAVEN_SERVICE_PROTECT, {page, 4096, PROT_READ | PROT_EXEC}, "return gate protection");
		state->mappings.at(page).protection = PROT_READ | PROT_EXEC;
		state->return_gate = page;
	}
	auto &registers = state->registers();
	registers.rax = registers.rbx = registers.rcx = registers.rdx = registers.rsi = registers.rdi = registers.rbp = 0;
	registers.r8 = registers.r9 = registers.r10 = registers.r11 = registers.r12 = registers.r13 = registers.r14 = registers.r15 = 0;
	registers.fs_base = registers.gs_base = 0;
	registers.fs = registers.gs = 0;
	registers.cs = 0x33;
	registers.orig_rax = uint64_t(-1);
	registers.eflags = 0x202;
	state->initial_context = context();
}

NativeProcess::~NativeProcess()
{
	if (std::getenv("RAVEN_PROFILE") && std::getenv("RAVEN_QUEUED_GRAPHICS"))
		std::cerr << "profile: queued calls=" << state->queued_calls << '\n';
	if (state->child && state->child->profile.enabled)
		std::fprintf(stderr, "controller: served=%llu served-trap-waits=%llu served-yields=%llu\n",
			static_cast<unsigned long long>(state->served_calls), static_cast<unsigned long long>(state->served_waits),
			static_cast<unsigned long long>(state->served_yields));
}

void NativeProcess::install_seccomp()
{
}

NativeContext NativeProcess::context() const
{
	state->require_stopped();
	NativeContext result{};
	std::memcpy(&result.registers, &state->registers(), sizeof result.registers);
	result.xstate.assign(state->child->thread().xstate, state->child->thread().xstate + RAVEN_XSTATE_SIZE);
	return result;
}

void NativeProcess::set_context(const NativeContext &context)
{
	state->require_stopped();
	// A context saved for a callback carries the general registers alone.
	if (!context.xstate.empty()) {
		if (context.xstate.size() < 512 || context.xstate.size() > RAVEN_XSTATE_SIZE)
			throw std::runtime_error("cannot restore native XSTATE");
		std::memcpy(state->child->thread().xstate, context.xstate.data(), context.xstate.size());
	}
	std::memcpy(&state->registers(), &context.registers, sizeof context.registers);
	state->thread_teb = context.registers.gs_base;
}

std::vector<uint8_t> NativeProcess::read_memory(uint64_t address, size_t size) const
{
	state->require_memory();
	auto &profile = state->child->profile;
	ControllerProfile::Scope timing(profile, profile.reads, size);
	if (!state->covers(address, size, PROT_READ))
		throw std::runtime_error("native read exceeds readable guest memory" +
			(std::getenv("RAVEN_TRACE_API") ? " at " + std::to_string(address) + "+" + std::to_string(size) : std::string()));
	std::vector<uint8_t> result(size);
	for (size_t offset = 0; offset < size;) {
		size_t count = size - offset;
		if (const auto *shared = state->shared_memory(address + offset, count)) {
			ControllerProfile::Scope timing(profile, profile.shared_reads, count);
			std::memcpy(result.data() + offset, shared, count);
			offset += count;
			continue;
		}
		count = std::min<size_t>(count, RAVEN_CONTROL_DATA_SIZE);
		mach_vm_size_t copied = 0;
		kern_return_t read;
		{
			ControllerProfile::Scope timing(profile, profile.mach_reads, count);
			read = mach_vm_read_overwrite(state->child->task, address + offset, count,
				reinterpret_cast<mach_vm_address_t>(result.data() + offset), &copied);
		}
		if (read != KERN_SUCCESS || copied != count) {
			// The kernel works in the host's 16 KiB pages; a range at the edge of a 4 KiB guest mapping goes through the agent.
			state->service(RAVEN_SERVICE_READ, {address + offset, count}, "memory read");
			std::memcpy(result.data() + offset, state->child->data, count);
		}
		offset += count;
	}
	return result;
}

void NativeProcess::write_memory(uint64_t address, std::span<const uint8_t> bytes)
{
	state->trap_page.clear();
	validate_write_memory(address, bytes.size());
	auto &profile = state->child->profile;
	ControllerProfile::Scope timing(profile, profile.writes, bytes.size());
	for (size_t offset = 0; offset < bytes.size();) {
		size_t count = bytes.size() - offset;
		// Executable shared views must still be changed through guest stores, so Rosetta invalidates their translation.
		const auto after = state->mappings.upper_bound(address + offset);
		const auto &mapping = std::prev(after)->second;
		count = std::min(count, size_t(mapping.address + mapping.size - address - offset));
		const bool code = mapping.protection & PROT_EXEC;
		if (auto *shared = state->shared_memory(address + offset, count); shared && !code) {
			ControllerProfile::Scope timing(profile, profile.shared_writes, count);
			std::memcpy(shared, bytes.data() + offset, count);
			offset += count;
			continue;
		}
		count = std::min<size_t>(count, RAVEN_CONTROL_DATA_SIZE);
		// Pages that may hold code are written by the guest itself, so Rosetta sees it change; others take the host's write.
		kern_return_t written = KERN_FAILURE;
		if (!code) {
			ControllerProfile::Scope timing(profile, profile.mach_writes, count);
			written = mach_vm_write(state->child->task, address + offset, reinterpret_cast<vm_offset_t>(bytes.data() + offset),
				mach_msg_type_number_t(count));
		}
		if (written != KERN_SUCCESS) {
			std::memcpy(state->child->data, bytes.data() + offset, count);
			state->service(RAVEN_SERVICE_WRITE, {address + offset, count, uint64_t(state->protection_at(address + offset))}, "memory write");
		}
		offset += count;
	}
}

void NativeProcess::validate_write_memory(uint64_t address, size_t size) const
{
	state->require_memory();
	if (!state->covers(address, size, PROT_WRITE))
		throw std::runtime_error("native write exceeds writable guest memory");
}

uint64_t NativeProcess::compare_exchange(uint64_t address, uint64_t expected, uint64_t desired, size_t size)
{
	if ((size != 4 && size != 8) || address % size)
		throw std::runtime_error("invalid native atomic memory operand");
	validate_write_memory(address, size);
	if (parallel())
		return state->service(RAVEN_SERVICE_COMPARE_EXCHANGE, {address, expected, desired, size}, "atomic exchange");
	// The guest is stopped: nothing else changes its memory meanwhile.
	const auto bytes = read_memory(address, size);
	uint64_t previous = 0;
	std::memcpy(&previous, bytes.data(), size);
	if (previous == (size == 4 ? uint32_t(expected) : expected))
		write_memory(address, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&desired), size));
	return previous;
}

uint64_t NativeProcess::memory_syscall(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t)
{
	throw std::runtime_error("native memory system calls are not used on macOS");
}

static void memory_range(uint64_t address, size_t size, int protection, uint64_t limit)
{
	if (!size || size % 4096 || size > (uint64_t(1) << 42) || address % 4096 ||
		address >= limit || size > limit - address || (protection & ~(PROT_READ | PROT_WRITE | PROT_EXEC)))
		throw std::runtime_error("invalid native memory range or protection");
}

uint64_t NativeProcess::map_memory(size_t size, int protection, uint64_t address)
{
	state->trap_page.clear();
	state->require_memory();
	memory_range(address, size, protection, state->limit());
	state->allocations.reserve(state->allocations.size() + 1);
	const uint64_t flags = address ? RAVEN_MAP_EXACT : state->mode == NativeMode::x86 ? RAVEN_MAP_LOW : 0;
	uint64_t mapped;
	try {
		mapped = state->service(RAVEN_SERVICE_MAP, {address, size, uint64_t(protection), flags}, "allocation");
	} catch (const std::system_error &) {
		throw std::runtime_error("native allocation: " + std::string(address ? "the address range is in use" : "no address range is free"));
	}
	if ((address && mapped != address) || mapped >= state->limit() || size > state->limit() - mapped) {
		state->child->stop();
		throw std::runtime_error("host returned an invalid native allocation address");
	}
	state->mappings.insert_or_assign(mapped, ImageMapping{mapped, size, 0, protection});
	state->allocations.emplace_back(mapped, size);
	return mapped;
}

uint64_t NativeProcess::map_shared(int descriptor, size_t size, uint64_t offset)
{
	return map_descriptor(descriptor, size, offset);
}

uint64_t NativeProcess::map_descriptor(int descriptor, size_t size, uint64_t offset)
{
	state->require_memory();
	memory_range(0, size, PROT_READ | PROT_WRITE, state->limit());
	const auto flags = fcntl(descriptor, F_GETFL);
	if (flags == -1 || (flags & O_ACCMODE) != O_RDWR || (offset & 4095) || offset > INT64_MAX || size > INT64_MAX - offset)
		throw std::runtime_error("invalid native mapping descriptor or offset");
	auto view = State::host_view(descriptor, size, offset);
	const auto address = map_file(descriptor, size, offset, true);
	try {
		state->shared_views.emplace(address, std::move(view));
	} catch (...) {
		unmap_memory(address);
		throw;
	}
	return address;
}

uint64_t NativeProcess::receive_descriptor(int, uint64_t)
{
	throw std::runtime_error("native descriptor transfer goes through the guest agent on macOS");
}

uint64_t NativeProcess::map_file(int descriptor, size_t size, uint64_t offset, bool)
{
	state->trap_page.clear();
	state->require_memory();
	memory_range(0, size, PROT_READ | PROT_WRITE, state->limit());
	state->allocations.reserve(state->allocations.size() + 1);
	// Windows places views on the 64 KiB allocation granularity, and programs round view addresses down to it.
	state->child->send_descriptor(descriptor);
	const uint64_t flags = state->mode == NativeMode::x86 ? RAVEN_MAP_LOW : RAVEN_MAP_GRANULAR;
	const auto mapped = state->service(RAVEN_SERVICE_MAP_DESCRIPTOR, {0, size, offset, flags, PROT_READ | PROT_WRITE}, "shared mapping");
	if (mapped >= state->limit() || size > state->limit() - mapped) {
		state->child->stop();
		throw std::runtime_error("host returned an invalid shared mapping address");
	}
	state->mappings.insert_or_assign(mapped, ImageMapping{mapped, size, 0, PROT_READ | PROT_WRITE});
	state->allocations.emplace_back(mapped, size);
	return mapped;
}

bool NativeProcess::has_protection(uint64_t address, size_t size, int protection) const
{
	const uint64_t end = address + size;
	while (address < end) {
		const auto after = state->mappings.upper_bound(address);
		if (after == state->mappings.begin())
			return false;
		const auto &mapping = std::prev(after)->second;
		if (address - mapping.address >= mapping.size || mapping.protection != protection)
			return false;
		address = mapping.address + mapping.size;
	}
	return true;
}

void NativeProcess::protect_memory(uint64_t address, size_t size, int protection)
{
	state->trap_page.clear();
	protect_range(address, size, protection, false);
}

void NativeProcess::protect_image(uint64_t address, size_t size, int protection)
{
	protect_range(address, size, protection, true);
}

void NativeProcess::protect_range(uint64_t address, size_t size, int protection, bool image)
{
	state->require_memory();
	memory_range(address, size, protection, state->limit());
	const auto owned = std::find_if(state->allocations.begin(), state->allocations.end(), [&](const auto &allocation) {
		return address >= allocation.first && address - allocation.first < allocation.second &&
			size <= allocation.second - (address - allocation.first);
	});
	if (owned == state->allocations.end() && image) {
		uint64_t cursor = address;
		while (cursor < address + size) {
			const auto after = state->mappings.upper_bound(cursor);
			if (after == state->mappings.begin() || cursor - std::prev(after)->second.address >= std::prev(after)->second.size)
				throw std::runtime_error("native protection range is outside a dynamic allocation");
			cursor = std::prev(after)->second.address + std::prev(after)->second.size;
		}
	} else if (owned == state->allocations.end()) {
		throw std::runtime_error("native protection range is outside a dynamic allocation");
	}
	const uint64_t end = address + size;
	auto first_touched = state->mappings.upper_bound(address);
	if (first_touched != state->mappings.begin() && std::prev(first_touched)->second.address + std::prev(first_touched)->second.size > address)
		--first_touched;
	auto past_touched = first_touched;
	std::vector<ImageMapping> pieces;
	for (; past_touched != state->mappings.end() && past_touched->second.address < end; ++past_touched) {
		const auto &mapping = past_touched->second;
		const uint64_t last = mapping.address + mapping.size;
		if (mapping.address < address)
			pieces.push_back({mapping.address, address - mapping.address, 0, mapping.protection});
		const uint64_t first = std::max(mapping.address, address);
		pieces.push_back({first, std::min(last, end) - first, 0, protection});
		if (last > end)
			pieces.push_back({end, last - end, 0, mapping.protection});
	}
	state->service(RAVEN_SERVICE_PROTECT, {address, size, uint64_t(protection)}, "protection");
	state->mappings.erase(first_touched, past_touched);
	for (const auto &piece : pieces)
		state->mappings.insert_or_assign(piece.address, piece);
}

void NativeProcess::unmap_memory(uint64_t address)
{
	state->trap_page.clear();
	state->require_memory();
	const auto owned = std::find_if(state->allocations.begin(), state->allocations.end(), [&](const auto &allocation) {
		return allocation.first == address;
	});
	if (owned == state->allocations.end())
		throw std::runtime_error("native release requires a dynamic allocation base");
	const uint64_t end = address + owned->second;
	state->service(RAVEN_SERVICE_UNMAP, {address, owned->second}, "release");
	state->queued_blocks.erase(address);
	if (const auto view = state->shared_views.find(address); view != state->shared_views.end())
		state->served_owners.erase(reinterpret_cast<uint64_t *>(view->second->host));
	state->shared_views.erase(address);
	state->mappings.erase(state->mappings.lower_bound(address), state->mappings.lower_bound(end));
	state->api_traps.erase(state->api_traps.lower_bound(address), state->api_traps.lower_bound(end));
	state->returning_traps.erase(state->returning_traps.lower_bound(address), state->returning_traps.lower_bound(end));
	state->allocations.erase(owned);
}

bool NativeProcess::memory_available(uint64_t address, size_t size) const
{
	state->require_memory();
	memory_range(address, size, PROT_NONE, state->limit());
	const auto end = address + size;
	const auto stack = state->initial_stack_top - state->initial_stack_size - 4096;
	if (address < state->initial_stack_top + 4096 && stack < end)
		return false;
	for (const auto &[start, mapping] : state->mappings)
		if (address < mapping.address + mapping.size && mapping.address < end)
			return false;
	// The agent, Rosetta and the libraries they load live in the guest's address space too; only the agent knows where.
	return state->service(RAVEN_SERVICE_AVAILABLE, {address, size}, "availability") != 0;
}

void NativeProcess::discard_memory(uint64_t address, size_t size)
{
	state->require_memory();
	memory_range(address, size, PROT_NONE, state->limit());
	const auto owned = std::find_if(state->allocations.begin(), state->allocations.end(), [&](const auto &allocation) {
		return address >= allocation.first && address - allocation.first < allocation.second &&
			size <= allocation.second - (address - allocation.first);
	});
	if (owned == state->allocations.end())
		throw std::runtime_error("native discard range is outside a dynamic allocation");
	// Linux's MADV_DONTNEED leaves private pages reading as zero; the agent maps fresh ones with the protection the pages had.
	const auto shared = state->shared_views.upper_bound(address);
	const bool view = shared != state->shared_views.begin() && address - std::prev(shared)->first < std::prev(shared)->second->size;
	for (uint64_t at = address; at < address + size;) {
		const auto found = state->mappings.upper_bound(at);
		uint64_t piece_end = address + size;
		int protection = PROT_NONE;
		if (found != state->mappings.begin() && at - std::prev(found)->second.address < std::prev(found)->second.size) {
			protection = std::prev(found)->second.protection;
			piece_end = std::min(piece_end, std::prev(found)->second.address + std::prev(found)->second.size);
		} else if (found != state->mappings.end()) {
			piece_end = std::min(piece_end, found->second.address);
		}
		state->service(RAVEN_SERVICE_DISCARD, {at, piece_end - at, uint64_t(protection), uint64_t(view)}, "discard");
		at = piece_end;
	}
}

void NativeProcess::map_image(LoadedImage &image)
{
	state->require_memory();
	const auto base = image.base();
	const auto size = image.data().size();
	memory_range(base, size, PROT_NONE, state->limit());
	const auto mappings = image.mappings();
	auto view = State::host_view(image.descriptor(), size);
	state->image_allocations.reserve(state->image_allocations.size() + 1);
	bool mapped = false;
	try {
		state->child->send_descriptor(image.descriptor());
		const auto result = state->service(RAVEN_SERVICE_MAP_DESCRIPTOR, {base, size, 0, RAVEN_MAP_EXACT, PROT_NONE}, "image mapping");
		if (result != base) {
			state->child->stop();
			throw std::runtime_error("host returned an invalid native image address");
		}
		mapped = true;
		for (const auto &mapping : mappings)
			state->service(RAVEN_SERVICE_PROTECT, {mapping.address, mapping.size, uint64_t(mapping.protection)}, "image protection");
		for (const auto &mapping : mappings)
			state->mappings.insert_or_assign(mapping.address, mapping);
		state->image_allocations.emplace_back(base, size);
		state->shared_views.emplace(base, std::move(view));
	} catch (const std::system_error &) {
		if (mapped)
			state->service(RAVEN_SERVICE_UNMAP, {base, size}, "image release");
		throw std::runtime_error("host returned an invalid native image address");
	} catch (...) {
		try {
			if (mapped)
				state->service(RAVEN_SERVICE_UNMAP, {base, size}, "image release");
		} catch (...) {
			state->child->stop();
		}
		throw;
	}
}

void NativeProcess::unmap_image(uint64_t address)
{
	state->require_memory();
	const auto owned = std::find_if(state->image_allocations.begin(), state->image_allocations.end(), [&](const auto &image) {
		return image.first == address;
	});
	if (owned == state->image_allocations.end())
		throw std::runtime_error("native image release requires a dynamically mapped image base");
	const uint64_t end = address + owned->second;
	state->service(RAVEN_SERVICE_UNMAP, {address, owned->second}, "image release");
	state->shared_views.erase(address);
	state->mappings.erase(state->mappings.lower_bound(address), state->mappings.lower_bound(end));
	state->api_traps.erase(state->api_traps.lower_bound(address), state->api_traps.lower_bound(end));
	state->returning_traps.erase(state->returning_traps.lower_bound(address), state->returning_traps.lower_bound(end));
	state->image_allocations.erase(owned);
}

std::pair<uint64_t, uint64_t> NativeProcess::stack_bounds() const
{
	state->require_stopped();
	return {state->initial_stack_top - state->initial_stack_size, state->initial_stack_top};
}

bool NativeProcess::active() const
{
	state->require_stopped();
	return state->active_call;
}

void NativeProcess::abandon_call()
{
	state->require_stopped();
	if (!state->active_call || !state->awaiting_api)
		throw std::runtime_error("native abandonment requires a pending API call");
	state->callbacks.clear();
	state->callback_cleanups.clear();
	state->active_call = false;
	state->awaiting_api = false;
	state->awaiting_syscall = false;
}

void NativeProcess::end_call()
{
	if (parallel() && !state->stopped) {
		state->child->pause_thread();
		state->stopped = true;
	}
	state->require_stopped();
	state->callbacks.clear();
	state->callback_cleanups.clear();
	state->active_call = false;
	state->awaiting_api = false;
	state->awaiting_syscall = false;
	state->translated_words.reset();
}

uint64_t NativeProcess::return_gate() const
{
	return state->mode == NativeMode::x86 ? state->return_gate : state->child->gate;
}

uint64_t NativeProcess::result_register() const
{
	state->require_stopped();
	return state->registers().rax;
}

size_t NativeProcess::callback_depth() const
{
	return state->callbacks.size();
}

NativeContext NativeProcess::callback_origin(size_t index) const
{
	state->require_stopped();
	if (index >= state->callbacks.size())
		throw std::runtime_error("native callback origin beyond the nesting");
	NativeContext origin = state->callbacks[index];
	origin.xstate = context().xstate;
	return origin;
}

void NativeProcess::trace_instructions(size_t count, std::function<void(uint64_t)> sink)
{
	state->require_stopped();
	state->steps_left = count;
	state->step_sink = std::move(sink);
}

void NativeProcess::watch_write(uint64_t address)
{
	state->require_stopped();
	if (address)
		throw std::runtime_error("hardware watchpoints are not available under Rosetta 2");
}

NativeThreadState NativeProcess::save_thread(bool snapshot_registers) const
{
	if (!parallel() || state->stopped)
		state->require_stopped();
	const bool in_task = parallel() && !snapshot_registers;
	NativeContext saved{};
	if (state->stopped) {
		if (in_task)
			std::memcpy(&saved.registers, &state->registers(), sizeof saved.registers);
		else
			saved = context();
	}
	if (!state->stopped && state->mode == NativeMode::x86) {
		saved.registers.fs = state->fs_selector;
		saved.registers.fs_base = state->fs_base;
	}
	return {std::move(saved), state->callbacks, state->callback_cleanups, state->active_call, state->awaiting_api, state->awaiting_syscall,
		state->debug_print_fault, state->pending_cleanup, state->stack_top, state->translated_words, state->translated_depth,
		state->translated_floats, state->translated_result, parallel() ? state->child->selected : 0, state->stopped,
		state->thread_served_guest, state->thread_served_block, state->serving, state->served_stack, in_task,
		state->serving_queued};
}

void NativeProcess::load_thread(const NativeThreadState &thread)
{
	if (parallel()) {
		state->child->select(thread.task);
		state->stopped = thread.stopped;
	} else {
		state->require_stopped();
	}
	if (state->stopped && !thread.context_in_task)
		set_context(thread.context);
	state->callbacks = thread.callbacks;
	state->callback_cleanups = thread.callback_cleanups;
	state->active_call = thread.active_call;
	state->awaiting_api = thread.awaiting_api;
	state->awaiting_syscall = thread.awaiting_syscall;
	state->debug_print_fault = thread.debug_print_fault;
	state->pending_cleanup = thread.pending_cleanup;
	state->stack_top = thread.stack_top;
	state->translated_words = thread.translated_words;
	state->translated_depth = thread.translated_depth;
	state->translated_floats = thread.translated_floats;
	state->translated_result = thread.translated_result;
	state->thread_served_guest = thread.served_guest;
	state->thread_served_block = thread.served_block;
	state->serving = thread.serving;
	state->serving_queued = thread.serving_queued;
	state->served_stack = thread.served_stack;
	state->slice_started = std::chrono::steady_clock::now();
	if (state->mode == NativeMode::x86) {
		state->fs_selector = uint32_t(thread.context.registers.fs);
		state->fs_base = thread.context.registers.fs_base;
	}
}

NativeThreadState NativeProcess::new_thread(uint64_t stack_top, uint64_t teb)
{
	state->require_memory();
	NativeThreadState thread{};
	thread.context = state->stopped ? context() : state->initial_context;
	if (state->mode == NativeMode::x86) {
		// Every thread owns an LDT entry, so a switch only reloads registers.
		const uint32_t entry = state->ldt_entries++;
		install_segment(entry, teb);
		thread.context.registers.fs = entry * 8 + 7;
		thread.context.registers.fs_base = teb;
	} else {
		thread.context.registers.gs_base = teb;
	}
	thread.stack_top = stack_top;
	if (parallel() && state->served_handler)
		make_served_block(teb, thread.served_guest, thread.served_block);
	if (parallel())
		thread.task = state->child->new_thread();
	return thread;
}

bool NativeProcess::parallel() const
{
	return state->child->parallel;
}

bool NativeProcess::stopped() const
{
	return state->stopped;
}

int NativeProcess::execution_descriptor() const
{
	return state->child->execution_descriptor();
}

void NativeProcess::drain_execution()
{
	state->child->drain_execution();
}

void NativeProcess::set_execution_wait(std::function<void(std::function<bool()>, std::chrono::steady_clock::time_point)> wait)
{
	state->execution_wait = std::move(wait);
}

void NativeProcess::stop_thread(const NativeThreadState &thread)
{
	if (parallel() && thread.task > 0 && thread.task != state->child->selected) {
		state->child->stop_thread(thread.task);
		drain_queued_calls(true);
	}
}

void NativeProcess::pause_thread(NativeThreadState &thread)
{
	if (!parallel() || thread.stopped)
		return;
	auto &child = *state->child;
	const pid_t caller = child.selected;
	const bool stopped = state->stopped;
	try {
		child.select(thread.task);
		child.pause_thread();
		state->stopped = true;
		drain_queued_calls(true);
		thread.context = context();
	} catch (...) {
		child.select(caller);
		state->stopped = stopped;
		throw;
	}
	child.select(caller);
	state->stopped = stopped;
}

void NativeProcess::enter_fault_dispatch()
{
	state->require_stopped();
	if (!state->active_call || state->awaiting_api || state->awaiting_syscall || state->mode != NativeMode::x64)
		throw std::runtime_error("native fault dispatch requires a stopped x64 call");
	state->awaiting_api = true;
	state->pending_cleanup = 0;
}

void NativeProcess::redirect(const NativeContext &target, size_t depth)
{
	state->require_stopped();
	if (!state->active_call || !state->awaiting_api || depth > state->callbacks.size())
		throw std::runtime_error("native redirection requires a pending API call");
	set_context(target);
	state->callbacks.resize(depth);
	state->callback_cleanups.resize(depth);
	state->awaiting_api = false;
	state->awaiting_syscall = false;
}

NativeMode NativeProcess::mode() const
{
	return state->mode;
}

void NativeProcess::set_teb(uint64_t base)
{
	state->require_stopped();
	install_segment(0, base);
	// The runtime's selector for LDT entry 0; the agent maps its entries to the ones Rosetta allows.
	state->fs_selector = 7;
	state->fs_base = base;
}

void NativeProcess::install_segment(uint32_t entry, uint64_t base)
{
	if (state->mode != NativeMode::x86 || base > 0xfffff000ULL || base % 4096)
		throw std::runtime_error("native TEB descriptor requires an x86 process and a page-aligned 32-bit address");
	state->service(RAVEN_SERVICE_SET_LDT, {entry + RAVEN_LDT_FIRST_TEB, base, 0xfff, 0}, "TEB descriptor");
}

void NativeProcess::add_api_trap(uint64_t address, uint32_t callee_cleanup)
{
	state->require_stopped();
	const auto byte_at = [&](uint64_t at) {
		const uint64_t page = at & ~uint64_t(4095);
		if (page != state->trap_page_address || state->trap_page.empty()) {
			state->trap_page = read_memory(page, state->covers(page, 4096, PROT_EXEC) ? 4096 : size_t(at - page + 1));
			state->trap_page_address = page;
		}
		return at - page < state->trap_page.size() ? state->trap_page[at - page] : read_memory(at, 1)[0];
	};
	if (!state->covers(address, 1, PROT_EXEC) || byte_at(address) != 0xcc)
		throw std::runtime_error("native API trap must be an executable INT3");
	if (state->mode == NativeMode::x86 && (address >= state->limit() || callee_cleanup % 4))
		throw std::runtime_error("native x86 API trap requires a 32-bit address and dword-aligned cleanup");
	state->api_traps[address] = callee_cleanup;
	if (state->mode != NativeMode::x86 && state->covers(address + 1, 1, PROT_EXEC) && byte_at(address + 1) == 0xc3)
		state->returning_traps.insert(address);
	else
		state->returning_traps.erase(address);
}

NativeResult NativeProcess::call(uint64_t address, std::span<const uint64_t> arguments, std::chrono::milliseconds timeout)
{
	state->require_stopped();
	if (state->active_call)
		throw std::runtime_error("native call is already active");
	return begin_call(address, arguments, state->stack_top, timeout);
}

NativeResult NativeProcess::callback(uint64_t address, std::span<const uint64_t> arguments, std::chrono::milliseconds timeout)
{
	state->require_stopped();
	if (!state->active_call || !state->awaiting_api) {
		backtrace_if_asked();
		throw std::runtime_error(std::string("native callback requires a pending API call (last API ") + api_in_progress + ")");
	}
	if (state->callbacks.size() >= 64)
		throw std::runtime_error("native callback exceeds initial nesting limit");
	NativeContext saved{};
	std::memcpy(&saved.registers, &state->registers(), sizeof saved.registers);
	state->callbacks.push_back(saved);
	state->callback_cleanups.push_back(state->pending_cleanup);
	try {
		return begin_call(address, arguments, saved.registers.rsp & ~uint64_t(15), timeout);
	} catch (...) {
		if (state->stopped && state->child->pid != -1) {
			set_context(saved);
			state->callbacks.pop_back();
			state->pending_cleanup = state->callback_cleanups.back();
			state->callback_cleanups.pop_back();
			state->awaiting_api = true;
		} else {
			state->child->stop();
		}
		throw;
	}
}

NativeResult NativeProcess::begin_call(uint64_t address, std::span<const uint64_t> arguments, uint64_t stack_ceiling,
	std::chrono::milliseconds timeout)
{
	if (arguments.size() > 128)
		throw std::runtime_error("native call exceeds initial argument limit");
	if (!state->covers(address, 1, PROT_EXEC))
		throw std::runtime_error("native entry is outside executable image memory");
	state->child->set_timeout(timeout);
	if (state->mode == NativeMode::x86)
		return begin_x86_call(address, arguments, stack_ceiling, timeout);
	const size_t extra = arguments.size() > 4 ? (arguments.size() - 4) * 8 : 0;
	const size_t frame_size = ((32 + extra + 15) & ~size_t(15)) + 8;
	if (stack_ceiling < frame_size)
		throw std::runtime_error("native call stack address underflows");
	const uint64_t rsp = stack_ceiling - frame_size;
	std::vector<uint8_t> frame(frame_size);
	const auto put = [&](size_t offset, uint64_t value) {
		for (size_t byte = 0; byte < 8; ++byte)
			frame[offset + byte] = static_cast<uint8_t>(value >> (byte * 8));
	};
	put(0, state->child->gate);
	for (size_t index = 4; index < arguments.size(); ++index)
		put(40 + (index - 4) * 8, arguments[index]);
	write_memory(rsp, frame);
	auto &registers = state->registers();
	registers.rax = 0;
	registers.rcx = arguments.size() > 0 ? arguments[0] : 0;
	registers.rdx = arguments.size() > 1 ? arguments[1] : 0;
	registers.r8 = arguments.size() > 2 ? arguments[2] : 0;
	registers.r9 = arguments.size() > 3 ? arguments[3] : 0;
	registers.orig_rax = uint64_t(-1);
	registers.eflags = 0x202;
	registers.rip = address;
	registers.rsp = rsp;
	registers.cs = 0x33;
	registers.fs = 0;
	state->active_call = true;
	state->awaiting_api = false;
	state->awaiting_syscall = false;
	state->debug_print_fault = false;
	return resume(timeout);
}

NativeResult NativeProcess::begin_x86_call(uint64_t address, std::span<const uint64_t> arguments, uint64_t stack_ceiling,
	std::chrono::milliseconds timeout)
{
	const auto fits = [](uint64_t value) { return value <= UINT32_MAX || (int64_t(value) < 0 && int64_t(value) >= INT32_MIN); };
	if (!std::all_of(arguments.begin(), arguments.end(), fits))
		throw std::runtime_error("native x86 call argument exceeds 32 bits");
	const uint64_t size = arguments.size() * 4;
	if (stack_ceiling > state->limit() || stack_ceiling < size + 16)
		throw std::runtime_error("native call stack address is invalid");
	const uint64_t first_argument = (stack_ceiling - size) & ~uint64_t(15);
	const uint64_t esp = first_argument - 4;
	std::vector<uint8_t> frame(4 + size);
	const auto put = [&](size_t offset, uint64_t value) {
		for (size_t byte = 0; byte < 4; ++byte)
			frame[offset + byte] = static_cast<uint8_t>(value >> (byte * 8));
	};
	put(0, state->return_gate);
	for (size_t index = 0; index < arguments.size(); ++index)
		put(4 + index * 4, arguments[index]);
	write_memory(esp, frame);
	auto &registers = state->registers();
	registers.rax = registers.rcx = registers.rdx = 0;
	registers.orig_rax = uint64_t(-1);
	registers.eflags = 0x202;
	registers.rip = address;
	registers.rsp = esp;
	registers.cs = 0x23;
	registers.ss = registers.ds = registers.es = 0x2b;
	registers.fs = state->fs_selector;
	registers.fs_base = state->fs_base;
	registers.gs = 0;
	registers.gs_base = 0;
	state->active_call = true;
	state->awaiting_api = false;
	state->awaiting_syscall = false;
	state->debug_print_fault = false;
	return resume(timeout);
}

NativeResult NativeProcess::resume(std::chrono::milliseconds timeout)
{
	state->require_stopped();
	if (!state->active_call || state->awaiting_api) {
		backtrace_if_asked();
		throw std::runtime_error(std::string("native process has no resumable call or awaits an API response (last API ") + api_in_progress + ")");
	}
	auto &child = *state->child;
	auto &thread = child.thread();
	drain_queued_calls(true);
	child.set_timeout(timeout);
	const auto execution_deadline = std::chrono::steady_clock::now() + timeout;
	int signal = 0, code = 0;
	uint64_t fault_address = 0;
	for (;;) {
		const bool slicing = !parallel() && state->slice.count() > 0 && state->slice_yield && state->slice_wanted && state->slice_wanted();
		std::chrono::steady_clock::duration period = state->slice;
		if (slicing && std::chrono::steady_clock::now() - state->slice_started >= state->slice) {
			if (inside_served_stub()) {
				++state->slice_deferrals;
				period = std::chrono::microseconds(100);
			} else {
				++state->slice_yield_requests;
				state->slice_yield();
				state->slice_started = std::chrono::steady_clock::now();
			}
		}
		const bool sampling = state->sample_period.count() > 0 && state->sampler;
		if (slicing || sampling)
			child.set_slice(slicing && sampling ? std::min<std::chrono::steady_clock::duration>(period, state->sample_period) :
				slicing ? period : state->sample_period);
		else
			child.clear_slice();
		if (state->resume_hook)
			state->resume_hook();
		child.resume(state->steps_left > 0);
		state->stopped = false;
		int status = 0;
		if (parallel() && state->execution_wait) {
			// Capture this channel: the scheduler can select and run other pthreads while this frame waits.
			uint64_t *const block = state->thread_served_block;
			const auto command = [block] { return block && __atomic_load_n(&block[0], __ATOMIC_ACQUIRE); };
			const auto queued = [block] {
				return block && __atomic_load_n(&block[ServedQueue::head], __ATOMIC_ACQUIRE) !=
					__atomic_load_n(&block[ServedQueue::tail], __ATOMIC_ACQUIRE);
			};
			const auto ready = [&] { return load(&thread.state) == RAVEN_THREAD_STOPPED || command() || queued(); };
			const auto until = sampling ? std::min(execution_deadline, std::chrono::steady_clock::now() + state->sample_period) : execution_deadline;
			while (load(&thread.state) != RAVEN_THREAD_STOPPED && std::chrono::steady_clock::now() < until) {
				const auto spin_until = std::min(until, std::chrono::steady_clock::now() + std::chrono::microseconds(5));
				while (!ready() && std::chrono::steady_clock::now() < spin_until) {}
				if (!ready())
					state->execution_wait(ready, until);
				if (command() || queued())
					serve_pending();
			}
			status = load(&thread.state) == RAVEN_THREAD_STOPPED ? 1 : sampling && until < execution_deadline ? -2 : -1;
		} else {
			status = child.wait(state->poll_served ? &state->poll_served : nullptr);
		}
		child.clear_slice();
		if (status == -2) {
			++state->slice_interrupts;
			// The guest ran a whole slice without a stop of its own: interrupt it where it is and let another thread have the processor.
			child.interrupt();
			child.set_timeout(std::chrono::seconds(2));
			status = child.wait();
			if (status == 1 && thread.signal == RAVEN_STOP_INTERRUPT) {
				state->stopped = true;
				if (sampling)
					state->sampler(thread.registers.rip);
				const bool wanted = state->slice.count() > 0 && state->slice_yield && state->slice_wanted && state->slice_wanted();
				if (wanted && std::chrono::steady_clock::now() - state->slice_started >= state->slice && !inside_served_stub()) {
					++state->slice_yield_requests;
					state->slice_yield();
					state->slice_started = std::chrono::steady_clock::now();
				} else if (!wanted) {
					state->slice_started = std::chrono::steady_clock::now();
				}
				child.set_timeout(timeout);
				continue;
			}
			// A stop of the guest's own came first; the interrupt it answered is spent.
		}
		if (status == -1) {
			if (std::getenv("RAVEN_TRACE_API")) {
				child.interrupt();
				child.set_timeout(std::chrono::seconds(1));
				if (child.wait() == 1)
					std::cerr << "raven: guest timeout at rip=0x" << std::hex << thread.registers.rip << " rsp=0x" << thread.registers.rsp
						<< std::dec << '\n';
			}
			child.stop();
			return {NativeStop::timeout, 0, 0, 0};
		}
		state->stopped = true;
		drain_queued_calls(true);
		auto &registers = thread.registers;
		signal = thread.signal;
		code = thread.code;
		fault_address = thread.fault_address;
		if (signal == RAVEN_STOP_INTERRUPT)
			continue;
		// macOS reports an access to a mapped page the protection forbids as SIGBUS; Linux, and the runtime, as SIGSEGV.
		if (signal == SIGBUS) {
			signal = SIGSEGV;
			code = SEGV_ACCERR;
		}
		// The vectors macOS uses for system calls (INT 0x80 to 0x83) stop with SIGILL after the instruction, not at it.
		if (signal == SIGILL && registers.rip >= 2 && state->covers(registers.rip - 2, 2, PROT_EXEC)) {
			const auto gate = read_memory(registers.rip - 2, 2);
			if (gate[0] == 0xcd && gate[1] >= 0x80 && gate[1] <= 0x83)
				registers.rip -= 2;
		}
		// An INT n without a gate is a general protection fault, which Linux reports as SIGSEGV from the kernel. Rosetta raises SIGILL for
		// some vectors (0x80) and SIGSEGV for others.
		if ((signal == SIGSEGV || signal == SIGILL) && state->covers(registers.rip, 2, PROT_EXEC) && read_memory(registers.rip, 1)[0] == 0xcd) {
			signal = SIGSEGV;
			code = SI_KERNEL;
			fault_address = 0;
		}
		if (signal == SIGSYS) {
			registers.orig_rax = registers.rax;
			signal = SIGTRAP | 0x80;
		}
		if (signal == SIGTRAP && served_wait_trap(registers.rip - 1)) {
			++state->served_waits;
			serve_pending();
			continue;
		}
		if (signal == SIGTRAP && std::binary_search(state->served_yield_traps.begin(), state->served_yield_traps.end(), registers.rip - 1)) {
			++state->served_yields;
			state->served_block[11] = 0;
			if (state->slice.count() > 0 && state->slice_yield && state->slice_wanted && state->slice_wanted()) {
				++state->slice_yield_requests;
				state->slice_yield();
			}
			state->slice_started = std::chrono::steady_clock::now();
			continue;
		}
		if (state->steps_left && signal == SIGTRAP) {
			const bool breakpoint = state->covers(registers.rip - 1, 1, PROT_EXEC) && read_memory(registers.rip - 1, 1)[0] == 0xcc;
			if (!breakpoint) {
				if (state->step_sink)
					state->step_sink(registers.rip);
				--state->steps_left;
				continue;
			}
		}
		break;
	}
	auto &registers = thread.registers;
	if (signal == (SIGTRAP | 0x80)) {
		state->awaiting_syscall = true;
		return {NativeStop::syscall, registers.orig_rax, registers.rip, signal};
	}
	state->awaiting_syscall = false;
	registers.orig_rax = uint64_t(-1);
	const bool x86 = state->mode == NativeMode::x86;
	const uint64_t gate = x86 ? state->return_gate : child.gate;
	if (signal == SIGTRAP && (registers.rip == gate + 1 || state->api_traps.contains(registers.rip - 1)))
		code = SI_KERNEL;
	state->debug_print_fault = signal == SIGSEGV && code == SI_KERNEL && registers.rax == 1;
	const bool breakpoint = signal == SIGTRAP && (code == TRAP_BRKPT || code == SI_KERNEL);
	if (breakpoint && registers.rip == gate + 1) {
		const auto value = x86 ? registers.rax & 0xffffffff : registers.rax;
		const auto rip = registers.rip;
		if (state->callbacks.empty()) {
			state->active_call = false;
		} else {
			set_context(state->callbacks.back());
			state->callbacks.pop_back();
			state->pending_cleanup = state->callback_cleanups.back();
			state->callback_cleanups.pop_back();
			state->awaiting_api = true;
		}
		return {NativeStop::returned, value, rip, signal};
	}
	if (breakpoint && state->api_traps.contains(registers.rip - 1)) {
		state->awaiting_api = true;
		state->pending_cleanup = state->api_traps.at(registers.rip - 1);
		return {NativeStop::api, registers.rip - 1, registers.rip, signal};
	}
	if (signal != SIGSEGV && signal != SIGBUS && signal != SIGILL && signal != SIGFPE)
		fault_address = 0;
	return {NativeStop::fault, registers.rax, registers.rip, signal, fault_address, code};
}

void NativeProcess::set_api_argument(size_t index, uint32_t value)
{
	state->require_stopped();
	if (!state->awaiting_api || state->mode != NativeMode::x86 || index > 127)
		throw std::runtime_error("native argument rewrite requires a pending x86 API call");
	const uint8_t bytes[4] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
	write_memory(state->registers().rsp + 4 + index * 4, bytes);
}

void NativeProcess::set_time_slicing(std::chrono::milliseconds slice, std::function<bool()> wanted, std::function<void()> yield)
{
	state->slice = slice;
	state->slice_wanted = std::move(wanted);
	state->slice_yield = std::move(yield);
}

void NativeProcess::set_guest_sampler(std::chrono::milliseconds period, std::function<void(uint64_t)> sampler)
{
	state->sample_period = period;
	state->sampler = std::move(sampler);
}

void NativeProcess::make_served_block(uint64_t teb, uint64_t &guest, uint64_t *&host)
{
	const int descriptor = memfd_create("raven-thread-calls", MFD_CLOEXEC);
	if (descriptor < 0)
		throw std::runtime_error("cannot create thread call block");
	try {
		if (ftruncate(descriptor, 4096) != 0)
			throw std::runtime_error("cannot size thread call block");
		guest = map_descriptor(descriptor, 4096);
		// Raven's extended TEB holds this thread's command block beyond the Windows fields.
		write_memory(teb + 0x1ff0, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(&guest), 8));
		host = reinterpret_cast<uint64_t *>(state->shared_views.at(guest)->host);
		state->served_owners.emplace(host, teb);
		if (state->mode == NativeMode::x64 && std::getenv("RAVEN_QUEUED_GRAPHICS")) {
			host[ServedQueue::enabled] = 1;
			state->queued_blocks.emplace(guest, host);
		}
	} catch (...) {
		close(descriptor);
		throw;
	}
	close(descriptor);
}

uint64_t NativeProcess::serving_teb() const
{
	if (!state->serving) return 0;
	if (!parallel()) return state->thread_teb;
	const auto found = state->served_owners.find(state->thread_served_block);
	if (found == state->served_owners.end()) throw std::runtime_error("served call has no thread-block owner");
	return found->second;
}

bool NativeProcess::serving_queued() const
{
	return state->serving && state->serving_queued;
}

void NativeProcess::set_queued_gl_elements(bool available)
{
	if (parallel() && state->thread_served_block)
		__atomic_store_n(&state->thread_served_block[ServedQueue::gl_elements], uint64_t(available), __ATOMIC_RELEASE);
}

void NativeProcess::set_served_calls(uint64_t *block, std::vector<uint64_t> stubs, size_t stub_size, size_t wait_offset,
	std::vector<uint64_t> yield_traps, std::function<void(uint64_t)> handler, std::vector<uint64_t> queued_stubs)
{
	std::sort(stubs.begin(), stubs.end());
	std::sort(yield_traps.begin(), yield_traps.end());
	std::sort(queued_stubs.begin(), queued_stubs.end());
	state->queued_stubs = std::move(queued_stubs);
	state->served_wait_offset = wait_offset;
	state->served_block = block;
	state->served_stubs = std::move(stubs);
	state->served_yield_traps = std::move(yield_traps);
	state->served_stub_size = stub_size;
	state->served_handler = std::move(handler);
	if (parallel() && state->served_handler && !state->thread_served_block)
		make_served_block(state->mode == NativeMode::x86 ? state->fs_base : context().registers.gs_base,
			state->thread_served_guest, state->thread_served_block);
	state->poll_served = [this] {
		if (!serve_pending())
			return false;
		// A served call can make another thread ready (a wake) without the guest stopping, so the guest gets a slice to be measured
		// against if it has none.
		// Give the acknowledged stub time to reach its own safe breakpoint before forcing a signal stop.
		const auto *block = parallel() ? state->thread_served_block : state->served_block;
		if (block[11])
			state->child->set_slice(state->slice);
		else if (state->slice.count() > 0 && state->slice_yield)
			state->child->ensure_slice(state->slice_started + state->slice);
		return true;
	};
}

bool NativeProcess::served_wait_trap(uint64_t address) const
{
	const auto after = std::upper_bound(state->served_stubs.begin(), state->served_stubs.end(), address);
	return after != state->served_stubs.begin() && address - *(after - 1) == state->served_wait_offset;
}

bool NativeProcess::inside_served_stub() const
{
	// Darwin libraries run on the one real guest thread with its own stack and TSD. A stopped unix call must resume on that thread.
	const auto &control = *state->child->control;
	const uint64_t instruction = state->registers().rip;
	if (load(&state->child->thread().unix_active) || (instruction >= control.unix_call_gate && instruction < control.unix_call_gate_end))
		return true;
	if (state->served_stubs.empty())
		return false;
	const uint64_t rip = state->registers().rip;
	if (std::binary_search(state->served_yield_traps.begin(), state->served_yield_traps.end(), rip - 1))
		return false;
	const auto after = std::upper_bound(state->served_stubs.begin(), state->served_stubs.end(), rip);
	return after != state->served_stubs.begin() && rip - *(after - 1) < state->served_stub_size;
}

uint64_t NativeProcess::unix_call_gate() const
{
	return state->mode == NativeMode::x64 ? state->child->control->unix_call_gate : 0;
}

uint64_t NativeProcess::load_unix_library(const std::string &path, uint32_t function_count)
{
	state->require_stopped();
	if (state->mode != NativeMode::x64 || !function_count || function_count > 4096 || path.empty() ||
		path.find('\0') != path.npos || path.size() >= RAVEN_CONTROL_DATA_SIZE)
		throw std::runtime_error("invalid x64 unix library request");
	std::memcpy(state->child->data, path.c_str(), path.size() + 1);
	return state->service(RAVEN_SERVICE_LOAD_UNIXLIB, {function_count}, "unix library load");
}

uint64_t NativeProcess::serve_call(uint64_t stub, std::vector<uint64_t> words, std::array<uint64_t, 4> floats, uint64_t stack, bool queued)
{
	++state->served_calls;
	auto translated_words = std::move(state->translated_words);
	const auto translated_depth = state->translated_depth;
	const auto translated_floats = state->translated_floats;
	const auto translated_result = state->translated_result;
	state->translated_words.reset();
	const bool was_serving = state->serving;
	const bool was_queued = state->serving_queued;
	const uint64_t was_stack = state->served_stack;
	state->serving = true;
	state->serving_queued = queued;
	state->served_stack = stack;
	const auto restore = [&] {
		state->translated_words = std::move(translated_words);
		state->translated_depth = translated_depth;
		state->translated_floats = translated_floats;
		state->translated_result = translated_result;
		state->serving = was_serving;
		state->serving_queued = was_queued;
		state->served_stack = was_stack;
	};
	uint64_t result = 0;
	try {
		begin_translated_call(std::move(words), floats);
		state->served_handler(stub);
		result = end_translated_call();
	} catch (...) {
		state->translated_words.reset();
		restore();
		throw;
	}
	restore();
	return result;
}

bool NativeProcess::queued_pending() const
{
	const auto *block = parallel() ? state->thread_served_block : nullptr;
	return block && __atomic_load_n(&block[ServedQueue::head], __ATOMIC_ACQUIRE) != block[ServedQueue::tail];
}

bool NativeProcess::drain_queued_calls(bool all)
{
	if (all) {
		// A caller may have observed another thread's atomic publication without a Windows wait. The served handler selects each
		// producer's TLS, so flush every thread before a synchronous operation can observe their commands.
		auto *current = state->thread_served_block;
		bool drained = false;
		try {
			for (const auto &[guest, block] : state->queued_blocks) {
				(void)guest;
				state->thread_served_block = block;
				drained |= drain_queued_calls();
			}
		} catch (...) {
			state->thread_served_block = current;
			throw;
		}
		state->thread_served_block = current;
		return drained;
	}
	if (!queued_pending())
		return false;
	auto *block = state->thread_served_block;
	const uint64_t head = __atomic_load_n(&block[ServedQueue::head], __ATOMIC_ACQUIRE);
	uint64_t tail = block[ServedQueue::tail];
	if (head - tail > ServedQueue::capacity)
		throw std::runtime_error("native served queue indices exceed capacity");
	// Consume a bounded snapshot. The producer may reuse a slot only after its arguments have been copied and served.
	while (tail != head) {
		const auto *record = block + ServedQueue::records + (tail % ServedQueue::capacity) * ServedQueue::words;
		const uint64_t stub = record[0];
		if (!std::binary_search(state->queued_stubs.begin(), state->queued_stubs.end(), stub))
			throw std::runtime_error("native served queue contains a synchronous or unknown call");
		serve_call(stub, {record[1], record[2], record[3], record[4], record[5], record[6], record[7], record[8]},
			{record[9], record[10], record[11], record[12]}, 0, true);
		++state->queued_calls;
		__atomic_store_n(&block[ServedQueue::tail], ++tail, __ATOMIC_RELEASE);
	}
	return true;
}

bool NativeProcess::serve_pending()
{
	const bool queued = drain_queued_calls();
	uint64_t *block = parallel() ? state->thread_served_block : state->served_block;
	if (!block)
		return queued;
	const uint64_t stub = __atomic_load_n(&block[0], __ATOMIC_ACQUIRE);
	if (!stub && !queued)
		return false;
	// A synchronous call observes every earlier queued record on this thread.
	if (stub) {
		drain_queued_calls(true);
		block[10] = serve_call(stub, {block[1], block[2], block[3], block[4]}, {block[6], block[7], block[8], block[9]}, block[5] + 40);
	}
	if (state->resume_hook)
		state->resume_hook();
	// The stub saves its result in RAX before obeying this request; later threads may then overwrite the shared result slot.
	const bool due = state->slice.count() > 0 && state->slice_yield &&
		std::chrono::steady_clock::now() - state->slice_started >= state->slice;
	block[11] = !parallel() && !state->served_yield_traps.empty() && due;
	if (stub)
		__atomic_store_n(&block[0], 0, __ATOMIC_RELEASE);
	// Parallel threads own their results, so another host frame can run as soon as this reply is published.
	if (parallel() && due) {
		state->slice_started = std::chrono::steady_clock::now();
		if (state->slice_wanted && state->slice_wanted())
			state->slice_yield();
	}
	return true;
}

void NativeProcess::set_resume_hook(std::function<void()> hook)
{
	state->resume_hook = std::move(hook);
}

void NativeProcess::begin_translated_call(std::vector<uint64_t> words, std::array<uint64_t, 4> floats)
{
	if (state->translated_words)
		throw std::runtime_error("a translated native call is already running");
	state->translated_words = std::move(words);
	state->translated_depth = state->callbacks.size();
	state->translated_floats = floats;
	state->translated_result = 0;
}

uint64_t NativeProcess::end_translated_call()
{
	state->translated_words.reset();
	return state->translated_result;
}

std::vector<uint64_t> NativeProcess::api_arguments(size_t count) const
{
	state->require_stopped();
	if (state->translated_words && state->callbacks.size() == state->translated_depth) {
		auto words = *state->translated_words;
		if (state->serving && count > words.size() && count <= 128) {
			const auto stack = read_memory(state->served_stack, (count - words.size()) * 8);
			for (size_t offset = 0; offset < stack.size(); offset += 8) {
				uint64_t value = 0;
				std::memcpy(&value, stack.data() + offset, 8);
				words.push_back(value);
			}
		}
		words.resize(count);
		return words;
	}
	if (!state->awaiting_api || count > 128)
		throw std::runtime_error("native process is not at an API or exceeds argument limit");
	const auto &registers = state->registers();
	if (state->mode == NativeMode::x86) {
		std::vector<uint64_t> values(count);
		if (!count)
			return values;
		const auto stack = read_memory(registers.rsp + 4, count * 4);
		for (size_t index = 0; index < count; ++index)
			for (size_t byte = 0; byte < 4; ++byte)
				values[index] |= uint64_t(stack[index * 4 + byte]) << (byte * 8);
		return values;
	}
	const uint64_t first[] = {registers.rcx, registers.rdx, registers.r8, registers.r9};
	std::vector<uint64_t> arguments(count);
	for (size_t index = 0; index < std::min(count, size_t(4)); ++index)
		arguments[index] = first[index];
	if (count > 4) {
		if (registers.rsp > UINT64_MAX - 40)
			throw std::runtime_error("native API stack address overflows");
		const auto stack = read_memory(registers.rsp + 40, (count - 4) * 8);
		for (size_t index = 4; index < count; ++index)
			for (size_t byte = 0; byte < 8; ++byte)
				arguments[index] |= uint64_t(stack[(index - 4) * 8 + byte]) << (byte * 8);
	}
	return arguments;
}

std::array<uint64_t, 4> NativeProcess::api_float_arguments() const
{
	state->require_stopped();
	if (state->translated_words && state->callbacks.size() == state->translated_depth)
		return state->translated_floats;
	if (!state->awaiting_api || state->mode != NativeMode::x64)
		throw std::runtime_error("native process is not at an x64 API call");
	std::array<uint64_t, 4> result{};
	// XMM0-3 in the FXSAVE image, from offset 160.
	for (size_t index = 0; index < 4; ++index)
		std::memcpy(&result[index], state->child->thread().xstate + 160 + index * 16, 8);
	return result;
}

void NativeProcess::enter_nt_service(uint64_t instruction, uint32_t number)
{
	state->require_stopped();
	if (state->mode != NativeMode::x64)
		throw std::runtime_error("native NT services are x64 only");
	if (!state->active_call || state->awaiting_api || !state->awaiting_syscall)
		throw std::runtime_error("native process is not at a pending syscall service");
	auto saved = context();
	if (saved.registers.rip != instruction || saved.registers.orig_rax != number || instruction < 2 ||
		read_memory(instruction - 2, 2) != std::vector<uint8_t>{0x0f, 0x05} || !state->covers(instruction - 2, 3, PROT_EXEC) ||
		read_memory(instruction, 1) != std::vector<uint8_t>{0xc3})
		throw std::runtime_error("native NT service boundary does not match the captured syscall");
	saved.registers.rcx = saved.registers.r10;
	saved.registers.orig_rax = uint64_t(-1);
	set_context(saved);
	state->awaiting_syscall = false;
	state->awaiting_api = true;
}

void NativeProcess::complete_debug_print(uint64_t instruction)
{
	state->require_stopped();
	if (state->mode != NativeMode::x64)
		throw std::runtime_error("native debug print services are x64 only");
	const auto saved = context();
	if (!state->active_call || state->awaiting_api || state->awaiting_syscall || !state->debug_print_fault ||
		saved.registers.rip != instruction || saved.registers.rax != 1 || !state->covers(instruction, 4, PROT_EXEC) ||
		read_memory(instruction, 4) != std::vector<uint8_t>{0xcd, 0x2d, 0xcc, 0xc3})
		throw std::runtime_error("native debug print does not match a captured INT 2D service");
	state->debug_print_fault = false;
	state->awaiting_api = true;
	return_api(0);
}

void NativeProcess::return_api(uint64_t value)
{
	state->require_stopped();
	if (state->translated_words && state->callbacks.size() == state->translated_depth) {
		state->translated_result = value;
		return;
	}
	if (!state->awaiting_api)
		throw std::runtime_error("native process is not awaiting an API result");
	auto &registers = state->registers();
	if (state->mode == NativeMode::x86) {
		if (std::getenv("RAVEN_TRACE_X86"))
			std::fprintf(stderr, "  -> %llx\n", static_cast<unsigned long long>(value & 0xffffffffULL));
		const auto stack = read_memory(registers.rsp, 4);
		uint64_t rip = 0;
		for (size_t byte = 0; byte < 4; ++byte)
			rip |= uint64_t(stack[byte]) << (byte * 8);
		registers.rip = rip;
		registers.rsp += 4 + state->pending_cleanup;
		registers.rax = value & 0xffffffff;
		registers.rdx = value >> 32;
		registers.orig_rax = uint64_t(-1);
		state->awaiting_api = false;
		return;
	}
	if (state->returning_traps.contains(registers.rip - 1)) {
		registers.rax = value;
		registers.orig_rax = uint64_t(-1);
		state->awaiting_api = false;
		return;
	}
	if (registers.rsp > UINT64_MAX - 8)
		throw std::runtime_error("native API return stack address overflows");
	const auto stack = read_memory(registers.rsp, 8);
	uint64_t rip = 0;
	for (size_t byte = 0; byte < 8; ++byte)
		rip |= uint64_t(stack[byte]) << (byte * 8);
	registers.rip = rip;
	registers.rsp += 8;
	registers.rax = value;
	registers.orig_rax = uint64_t(-1);
	state->awaiting_api = false;
}

// Same as the Linux controller's: only NativeProcess is platform-specific.
static NativeResult execute(const PEImage &image, LoadedImage &loaded, std::span<LoadedImage *const> images,
	const std::string &bootstrap, std::chrono::milliseconds timeout)
{
	NativeProcess process(images, bootstrap, timeout);
	return process.call(loaded.base() + image.entry, {}, timeout);
}

NativeResult run_native_entry(const PEImage &image, LoadedImage &loaded,
	const std::string &bootstrap, std::chrono::milliseconds timeout)
{
	if (!image.is_64)
		throw std::runtime_error("native PE32 execution is not implemented");
	if ((image.characteristics & 0x2000) != 0 || !image.imports().empty() ||
		image.directories[9].size != 0 || image.directories[13].size != 0 || image.directories[14].size != 0)
		throw std::runtime_error("native entry requires an executable without imports, TLS, delayed imports or CLR initialization");
	LoadedImage *images[] = {&loaded};
	return execute(image, loaded, images, bootstrap, timeout);
}

NativeResult run_native_modules(ModuleSet &modules, PEModule &entry,
	const std::string &bootstrap, std::chrono::milliseconds timeout)
{
	if ((entry.image.characteristics & 0x2000) != 0 ||
		!std::any_of(modules.all().begin(), modules.all().end(), [&](const auto &item) { return item.second.get() == &entry; }))
		throw std::runtime_error("native module entry must be an executable in the supplied graph");
	std::vector<LoadedImage *> images;
	for (const auto &[name, module] : modules.all()) {
		if (!module->image.is_64)
			throw std::runtime_error("native PE32 execution is not implemented");
		if (module->image.directories[9].size || module->image.directories[13].size || module->image.directories[14].size ||
			((module->image.characteristics & 0x2000) != 0 && module->image.entry))
			throw std::runtime_error("native module requires unsupported DLL, TLS, delayed import or CLR initialization: " + name);
		images.push_back(&module->loaded);
	}
	modules.bind_imports();
	return execute(entry.image, entry.loaded, images, bootstrap, timeout);
}

}
