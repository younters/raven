// The Linux interfaces of the shim headers, on macOS. Names in parentheses, as (socket)(...), are the system functions the shim
// macros stand in front of.

#include <dirent.h>
#include <dispatch/dispatch.h>
#include <mach-o/dyld.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <libproc.h>
#include <net/if_dl.h>
#include <linux/futex.h>
#include <mach/mach.h>
#include <os/os_sync_wait_on_address.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/sysinfo.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string temporary_directory()
{
	char path[PATH_MAX];
	if (confstr(_CS_DARWIN_USER_TEMP_DIR, path, sizeof path) > 0)
		return path;
	const char *directory = getenv("TMPDIR");
	return directory && *directory ? directory : "/tmp/";
}

std::string unique_path(const char *kind)
{
	static std::atomic<unsigned> counter{0};
	auto directory = temporary_directory();
	if (directory.back() != '/')
		directory += '/';
	return directory + "raven-" + kind + "-" + std::to_string(getpid()) + "-" + std::to_string(counter.fetch_add(1)) + "-" +
		std::to_string(arc4random());
}

int set_flags(int descriptor, bool close_on_exec, bool nonblocking)
{
	if (close_on_exec && fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0)
		return -1;
	if (nonblocking) {
		const int flags = fcntl(descriptor, F_GETFL, 0);
		if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0)
			return -1;
	}
	return 0;
}

int no_sigpipe(int descriptor)
{
	const int one = 1;
	// Only sockets have the option; anything else keeps working as it would.
	setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
	return descriptor;
}

// macOS has no SOCK_SEQPACKET for Unix sockets: such a socket is a stream socket that frames its messages (see sockets.cpp).
bool sequenced_packets(int domain, int type)
{
	return domain == AF_UNIX && (type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK)) == SOCK_SEQPACKET;
}

int fail(int descriptor)
{
	const int error = errno;
	close(descriptor);
	errno = error;
	return -1;
}

// A FIFO opened for both reading and writing through one descriptor, and its name removed.
int open_fifo(int flags)
{
	const auto path = unique_path("fifo");
	if (mkfifo(path.c_str(), 0600) != 0)
		return -1;
	const int descriptor = open(path.c_str(), O_RDWR | O_NONBLOCK | ((flags & O_CLOEXEC) ? O_CLOEXEC : 0));
	const int error = errno;
	unlink(path.c_str());
	if (descriptor < 0) {
		errno = error;
		return -1;
	}
	if (!(flags & O_NONBLOCK)) {
		const int status = fcntl(descriptor, F_GETFL, 0);
		if (status < 0 || fcntl(descriptor, F_SETFL, status & ~O_NONBLOCK) < 0)
			return fail(descriptor);
	}
	return descriptor;
}

// inotify. An instance is a datagram socket pair: the runtime reads the one end, and every record is one datagram written to the other,
// so a read takes whole records as it does on Linux. A watch is a vnode source on the directory and the entries it held when last looked
// at; when the directory changes, its entries are listed again and the differences become records. All of it runs on one serial queue.
struct Entry {
	ino_t inode;
	bool directory;
	timespec modified, changed;
	off_t size;
};

struct NotifyInstance;

struct DirectoryWatch {
	NotifyInstance *instance;
	int directory;
	uint32_t mask;
	dispatch_source_t source;
	std::map<std::string, Entry> entries;
};

struct NotifyInstance {
	int writer;
	bool overflowed = false;
	uint32_t cookie = 0;
	std::map<int, DirectoryWatch *> watches;
};

dispatch_queue_t notify_queue()
{
	static const dispatch_queue_t queue = dispatch_queue_create("raven.inotify", DISPATCH_QUEUE_SERIAL);
	return queue;
}

// The instances by the descriptor the runtime reads; touched on the notify queue only.
std::map<int, NotifyInstance *> &notify_instances()
{
	static auto *instances = new std::map<int, NotifyInstance *>;
	return *instances;
}

std::map<std::string, Entry> list_entries(int directory)
{
	std::map<std::string, Entry> entries;
	const int listing = openat(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (listing < 0)
		return entries;
	DIR *stream = fdopendir(listing);
	if (!stream) {
		close(listing);
		return entries;
	}
	while (const dirent *item = readdir(stream)) {
		if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, ".."))
			continue;
		struct stat status{};
		if (fstatat(dirfd(stream), item->d_name, &status, AT_SYMLINK_NOFOLLOW) == 0)
			entries[item->d_name] = {status.st_ino, S_ISDIR(status.st_mode), status.st_mtimespec, status.st_ctimespec, status.st_size};
	}
	closedir(stream);
	return entries;
}

void release_watch(void *context)
{
	auto *watch = static_cast<DirectoryWatch *>(context);
	close(watch->directory);
	delete watch;
}

void drop_instance(NotifyInstance *instance)
{
	for (const auto &[wd, watch] : instance->watches)
		dispatch_source_cancel(watch->source);
	close(instance->writer);
	for (auto item = notify_instances().begin(); item != notify_instances().end(); ++item)
		if (item->second == instance) {
			notify_instances().erase(item);
			break;
		}
	delete instance;
}

// Sends one record; false when the reading end has gone, and the instance with it. A full buffer loses the record, and the next one
// that fits is preceded by IN_Q_OVERFLOW, as Linux reports a full queue.
bool send_record(NotifyInstance *instance, int wd, uint32_t mask, uint32_t cookie, const std::string &name)
{
	const auto send_one = [instance](int target, uint32_t bits, uint32_t pairing, const std::string &text) {
		const size_t length = text.empty() ? 0 : (text.size() + 1 + 3) & ~size_t(3);
		std::vector<char> record(sizeof(inotify_event) + length, 0);
		auto *event = reinterpret_cast<inotify_event *>(record.data());
		event->wd = target;
		event->mask = bits;
		event->cookie = pairing;
		event->len = uint32_t(length);
		memcpy(record.data() + sizeof(inotify_event), text.data(), text.size());
		return send(instance->writer, record.data(), record.size(), 0) == ssize_t(record.size()) ? 0 : errno;
	};
	if (instance->overflowed) {
		const int error = send_one(-1, IN_Q_OVERFLOW, 0, {});
		if (error == EAGAIN || error == ENOBUFS)
			return true;
		if (error)
			return false;
		instance->overflowed = false;
	}
	const int error = send_one(wd, mask, cookie, name);
	if (error == EAGAIN || error == ENOBUFS)
		instance->overflowed = true;
	return !error || error == EAGAIN || error == ENOBUFS;
}

void directory_changed(void *context)
{
	auto *watch = static_cast<DirectoryWatch *>(context);
	auto *instance = watch->instance;
	const int wd = watch->directory;
	const auto happened = dispatch_source_get_data(watch->source);
	bool alive = true;
	const auto report = [&](uint32_t bits, uint32_t cookie, const std::string &name) {
		if (alive && (watch->mask & bits & ~IN_ISDIR) && !send_record(instance, wd, (bits & ~IN_ISDIR & watch->mask) | (bits & IN_ISDIR), cookie, name))
			alive = false;
	};
	if (happened & DISPATCH_VNODE_DELETE) {
		report(IN_DELETE_SELF, 0, {});
		if (alive)
			send_record(instance, wd, IN_IGNORED, 0, {});
		instance->watches.erase(wd);
		dispatch_source_cancel(watch->source);
		if (!alive)
			drop_instance(instance);
		return;
	}
	if (happened & DISPATCH_VNODE_RENAME)
		report(IN_MOVE_SELF, 0, {});
	auto now = list_entries(watch->directory);
	std::map<ino_t, std::string> gone;
	for (const auto &[name, before] : watch->entries) {
		const auto found = now.find(name);
		if (found == now.end() || found->second.inode != before.inode)
			gone[before.inode] = name;
	}
	for (const auto &[name, after] : now) {
		const auto found = watch->entries.find(name);
		const uint32_t kind = after.directory ? IN_ISDIR : 0;
		if (found != watch->entries.end() && found->second.inode == after.inode) {
			const auto &before = found->second;
			if (before.size != after.size || before.modified.tv_sec != after.modified.tv_sec || before.modified.tv_nsec != after.modified.tv_nsec)
				report(IN_MODIFY | kind, 0, name);
			else if (before.changed.tv_sec != after.changed.tv_sec || before.changed.tv_nsec != after.changed.tv_nsec)
				report(IN_ATTRIB | kind, 0, name);
			continue;
		}
		// The same file under a new name moved here; anything else is new.
		if (const auto moved = gone.find(after.inode); moved != gone.end()) {
			const uint32_t cookie = ++instance->cookie;
			report(IN_MOVED_FROM | kind, cookie, moved->second);
			report(IN_MOVED_TO | kind, cookie, name);
			gone.erase(moved);
		} else {
			report(IN_CREATE | kind, 0, name);
		}
	}
	for (const auto &[inode, name] : gone)
		report(IN_DELETE | (watch->entries[name].directory ? IN_ISDIR : 0), 0, name);
	watch->entries = std::move(now);
	if (!alive)
		drop_instance(instance);
}

}

std::filesystem::path raven_darwin::executable_path()
{
	uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::string path(size, '\0');
	if (_NSGetExecutablePath(path.data(), &size) != 0)
		throw std::filesystem::filesystem_error("executable path", std::make_error_code(std::errc::no_such_file_or_directory));
	path.resize(strlen(path.c_str()));
	// /proc/self/exe names the file itself, with no symbolic links on the way.
	return std::filesystem::canonical(path);
}

extern "C" {

ssize_t raven_darwin_readlink(const char *path, char *buffer, size_t size)
{
	// /proc/<pid>/exe: the program another process runs, by the same canonical path.
	const pid_t process = raven_darwin::proc_process(path, "exe");
	if (process < 0)
		return (readlink)(path, buffer, size);
	if (process != getpid()) {
		char found[PROC_PIDPATHINFO_MAXSIZE], resolved[PATH_MAX];
		if (proc_pidpath(process, found, sizeof found) <= 0) {
			errno = ENOENT;
			return -1;
		}
		const char *name = realpath(found, resolved) ? resolved : found;
		const size_t length = std::min(size, strlen(name));
		memcpy(buffer, name, length);
		return ssize_t(length);
	}
	try {
		const auto self = raven_darwin::executable_path().string();
		const size_t length = std::min(size, self.size());
		memcpy(buffer, self.data(), length);
		return ssize_t(length);
	} catch (const std::filesystem::filesystem_error &failure) {
		errno = failure.code().value();
		return -1;
	}
}

int memfd_create(const char *, unsigned int flags)
{
	auto path = unique_path("memfd");
	path += "-XXXXXX";
	const int descriptor = mkostemp(path.data(), (flags & MFD_CLOEXEC) ? O_CLOEXEC : 0);
	if (descriptor < 0)
		return -1;
	unlink(path.c_str());
	return descriptor;
}

int inotify_init1(int flags)
{
	int ends[2];
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, ends) != 0)
		return -1;
	// Room for a burst of records before the queue overflows.
	const int room = 256 << 10;
	setsockopt(ends[0], SOL_SOCKET, SO_RCVBUF, &room, sizeof room);
	setsockopt(ends[1], SOL_SOCKET, SO_SNDBUF, &room, sizeof room);
	no_sigpipe(ends[1]);
	if (set_flags(ends[0], flags & O_CLOEXEC, flags & O_NONBLOCK) != 0 || set_flags(ends[1], true, true) != 0) {
		close(ends[1]);
		return fail(ends[0]);
	}
	auto *instance = new NotifyInstance{ends[1]};
	dispatch_sync_f(notify_queue(), new std::pair<int, NotifyInstance *>(ends[0], instance), [](void *context) {
		auto *added = static_cast<std::pair<int, NotifyInstance *> *>(context);
		// A descriptor number taken again means the instance it belonged to was closed.
		if (const auto old = notify_instances().find(added->first); old != notify_instances().end())
			drop_instance(old->second);
		notify_instances()[added->first] = added->second;
		delete added;
	});
	return ends[0];
}

int inotify_add_watch(int descriptor, const char *path, uint32_t mask)
{
	struct stat status{};
	if (stat(path, &status) != 0)
		return -1;
	if ((mask & IN_ONLYDIR) && !S_ISDIR(status.st_mode)) {
		errno = ENOTDIR;
		return -1;
	}
	const int directory = open(path, O_EVTONLY | O_CLOEXEC);
	if (directory < 0)
		return -1;
	struct Request { int descriptor, directory; uint32_t mask; int result, error; } request{descriptor, directory, mask, -1, EBADF};
	dispatch_sync_f(notify_queue(), &request, [](void *context) {
		auto *asked = static_cast<Request *>(context);
		const auto found = notify_instances().find(asked->descriptor);
		if (found == notify_instances().end())
			return;
		auto *watch = new DirectoryWatch{found->second, asked->directory, asked->mask, nullptr, list_entries(asked->directory)};
		watch->source = dispatch_source_create(DISPATCH_SOURCE_TYPE_VNODE, uintptr_t(asked->directory),
			DISPATCH_VNODE_WRITE | DISPATCH_VNODE_DELETE | DISPATCH_VNODE_RENAME | DISPATCH_VNODE_ATTRIB | DISPATCH_VNODE_EXTEND, notify_queue());
		if (!watch->source) {
			delete watch;
			asked->error = ENOMEM;
			return;
		}
		dispatch_set_context(watch->source, watch);
		dispatch_source_set_event_handler_f(watch->source, directory_changed);
		dispatch_source_set_cancel_handler_f(watch->source, release_watch);
		dispatch_activate(watch->source);
		found->second->watches[asked->directory] = watch;
		asked->result = asked->directory;
	});
	if (request.result < 0) {
		close(directory);
		errno = request.error;
	}
	return request.result;
}

int inotify_rm_watch(int descriptor, int wd)
{
	struct Request { int descriptor, wd, result; } request{descriptor, wd, -1};
	dispatch_sync_f(notify_queue(), &request, [](void *context) {
		auto *asked = static_cast<Request *>(context);
		const auto found = notify_instances().find(asked->descriptor);
		if (found == notify_instances().end())
			return;
		const auto watch = found->second->watches.find(asked->wd);
		if (watch == found->second->watches.end())
			return;
		dispatch_source_cancel(watch->second->source);
		found->second->watches.erase(watch);
		send_record(found->second, asked->wd, IN_IGNORED, 0, {});
		asked->result = 0;
	});
	if (request.result < 0)
		errno = EINVAL;
	return request.result;
}

long raven_darwin_futex(void *word, int operation, uint32_t value, const timespec *timeout)
{
	const bool shared = !(operation & FUTEX_PRIVATE_FLAG);
	switch (operation & ~FUTEX_PRIVATE_FLAG) {
	case FUTEX_WAIT: {
		const auto flags = shared ? OS_SYNC_WAIT_ON_ADDRESS_SHARED : OS_SYNC_WAIT_ON_ADDRESS_NONE;
		if (!timeout) {
			if (os_sync_wait_on_address(word, value, sizeof(uint32_t), flags) < 0 && errno == EFAULT)
				return -1;
			return 0;
		}
		constexpr uint64_t second = 1'000'000'000;
		if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 || timeout->tv_nsec >= long(second)) {
			errno = EINVAL;
			return -1;
		}
		const auto seconds = uint64_t(timeout->tv_sec);
		const auto nanoseconds = uint64_t(timeout->tv_nsec);
		uint64_t duration = seconds > (UINT64_MAX - nanoseconds) / second ?
			UINT64_MAX : seconds * second + nanoseconds;
		// Darwin requires a positive relative timeout, even for an immediate Linux wait.
		if (!duration)
			duration = 1;
		const int result = os_sync_wait_on_address_with_timeout(word, value, sizeof(uint32_t), flags,
			OS_CLOCK_MACH_ABSOLUTE_TIME, duration);
		return result < 0 ? -1 : 0;
	}
	case FUTEX_WAKE: {
		const auto flags = shared ? OS_SYNC_WAKE_BY_ADDRESS_SHARED : OS_SYNC_WAKE_BY_ADDRESS_NONE;
		if (os_sync_wake_by_address_all(word, sizeof(uint32_t), flags) < 0 && errno != ENOENT)
			return -1;
		return 0;
	}
	}
	errno = ENOSYS;
	return -1;
}

int sysinfo(struct sysinfo *info)
{
	memset(info, 0, sizeof *info);
	info->mem_unit = 1;
	uint64_t memory = 0;
	size_t size = sizeof memory;
	if (sysctlbyname("hw.memsize", &memory, &size, nullptr, 0) != 0)
		return -1;
	info->totalram = memory;
	vm_statistics64_data_t statistics{};
	mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
	vm_size_t page = 0;
	host_page_size(mach_host_self(), &page);
	if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&statistics), &count) == KERN_SUCCESS) {
		info->freeram = uint64_t(statistics.free_count) * page;
		// What macOS can give back without swapping: inactive, purgeable and file-backed pages.
		info->bufferram = (uint64_t(statistics.inactive_count) + statistics.purgeable_count + statistics.external_page_count) * page;
	}
	xsw_usage swap{};
	size = sizeof swap;
	if (sysctlbyname("vm.swapusage", &swap, &size, nullptr, 0) == 0) {
		info->totalswap = swap.xsu_total;
		info->freeswap = swap.xsu_avail;
	}
	timeval boot{};
	size = sizeof boot;
	if (sysctlbyname("kern.boottime", &boot, &size, nullptr, 0) == 0)
		info->uptime = long(time(nullptr) - boot.tv_sec);
	double loads[3];
	if (getloadavg(loads, 3) == 3)
		for (int i = 0; i < 3; i++)
			info->loads[i] = (unsigned long)(loads[i] * 65536.0);
	return 0;
}

ssize_t getrandom(void *buffer, size_t length, unsigned int)
{
	arc4random_buf(buffer, length);
	return ssize_t(length);
}

int sched_getaffinity(pid_t, size_t size, cpu_set_t *set)
{
	if (size < sizeof(cpu_set_t)) {
		errno = EINVAL;
		return -1;
	}
	CPU_ZERO(set);
	int processors = 0;
	size_t length = sizeof processors;
	if (sysctlbyname("hw.logicalcpu", &processors, &length, nullptr, 0) != 0 || processors <= 0)
		processors = 1;
	for (int cpu = 0; cpu < processors && cpu < CPU_SETSIZE; cpu++)
		CPU_SET(cpu, set);
	return 0;
}

long raven_darwin_sysconf(int name)
{
	if (name == _SC_PAGESIZE)
		return 4096;
	if (name == _SC_PHYS_PAGES) {
		const long pages = (sysconf)(_SC_PHYS_PAGES), host = (sysconf)(_SC_PAGESIZE);
		return pages > 0 && host > 0 ? pages * (host / 4096) : pages;
	}
	return (sysconf)(name);
}

int pipe2(int descriptors[2], int flags)
{
	if (pipe(descriptors) != 0)
		return -1;
	for (int i = 0; i < 2; i++)
		if (set_flags(descriptors[i], flags & O_CLOEXEC, flags & O_NONBLOCK) != 0) {
			const int error = errno;
			close(descriptors[0]);
			close(descriptors[1]);
			errno = error;
			return -1;
		}
	return 0;
}

int dup3(int from, int to, int flags)
{
	if (from == to) {
		errno = EINVAL;
		return -1;
	}
	if (dup2(from, to) < 0)
		return -1;
	if (set_flags(to, flags & O_CLOEXEC, false) != 0)
		return -1;
	return to;
}

int raven_darwin_socket(int domain, int type, int protocol)
{
	const bool packets = sequenced_packets(domain, type);
	const int descriptor = (socket)(domain, packets ? SOCK_STREAM : type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK), protocol);
	if (descriptor < 0)
		return -1;
	if (set_flags(descriptor, type & SOCK_CLOEXEC, type & SOCK_NONBLOCK) != 0 || (packets && raven_darwin_frame_socket(descriptor) != 0))
		return fail(descriptor);
	return no_sigpipe(descriptor);
}

int raven_darwin_socketpair(int domain, int type, int protocol, int descriptors[2])
{
	const bool packets = sequenced_packets(domain, type);
	if ((socketpair)(domain, packets ? SOCK_STREAM : type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK), protocol, descriptors) != 0)
		return -1;
	for (int i = 0; i < 2; i++) {
		if (set_flags(descriptors[i], type & SOCK_CLOEXEC, type & SOCK_NONBLOCK) != 0 ||
			(packets && raven_darwin_frame_socket(descriptors[i]) != 0)) {
			const int error = errno;
			close(descriptors[0]);
			close(descriptors[1]);
			errno = error;
			return -1;
		}
		no_sigpipe(descriptors[i]);
	}
	return 0;
}

int accept4(int descriptor, struct sockaddr *address, socklen_t *length, int flags)
{
	const int accepted = accept(descriptor, address, length);
	if (accepted < 0)
		return -1;
	// A connection takes the listener's send buffer but not its low-water mark, which makes it a sequenced-packet socket.
	if (set_flags(accepted, flags & SOCK_CLOEXEC, flags & SOCK_NONBLOCK) != 0 ||
		(raven_darwin_framed_socket(descriptor) && raven_darwin_frame_socket(accepted) != 0))
		return fail(accepted);
	return no_sigpipe(accepted);
}

int raven_darwin_peer_credentials(int descriptor, struct ucred *credentials, socklen_t *length)
{
	if (!length || *length < sizeof(ucred)) {
		errno = EINVAL;
		return -1;
	}
	pid_t pid = 0;
	socklen_t pid_length = sizeof pid;
	if (getsockopt(descriptor, SOL_LOCAL, LOCAL_PEERPID, &pid, &pid_length) != 0)
		return -1;
	uid_t uid;
	gid_t gid;
	if (getpeereid(descriptor, &uid, &gid) != 0)
		return -1;
	*credentials = {pid, uid, gid};
	*length = sizeof(ucred);
	return 0;
}

int raven_darwin_hardware_address(struct ifreq *request)
{
	ifaddrs *list = nullptr;
	if (getifaddrs(&list) != 0)
		return -1;
	int result = -1;
	errno = ENXIO;
	for (const auto *entry = list; entry; entry = entry->ifa_next)
		if (entry->ifa_addr && entry->ifa_addr->sa_family == AF_LINK && !strncmp(entry->ifa_name, request->ifr_name, IFNAMSIZ)) {
			const auto *link = reinterpret_cast<const sockaddr_dl *>(entry->ifa_addr);
			if (link->sdl_alen != 6)
				continue;
			memset(&request->ifr_hwaddr, 0, sizeof request->ifr_hwaddr);
			request->ifr_hwaddr.sa_family = AF_LINK;
			memcpy(request->ifr_hwaddr.sa_data, LLADDR(link), 6);
			result = 0;
			break;
		}
	freeifaddrs(list);
	return result;
}

}
