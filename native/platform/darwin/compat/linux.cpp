// The Linux interfaces of the shim headers, on macOS. Names in parentheses, as (socket)(...), are the system functions the shim
// macros stand in front of.

#include <dispatch/dispatch.h>
#include <mach-o/dyld.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
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
#include <string>

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

struct DirectoryWatch {
	int notify;
	int directory;
	dispatch_source_t source;
};

void directory_changed(void *context)
{
	const auto *watch = static_cast<DirectoryWatch *>(context);
	const uint64_t one = 1;
	[[maybe_unused]] const auto written = write(watch->notify, &one, sizeof one);
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
	if (strcmp(path, "/proc/self/exe") != 0)
		return (readlink)(path, buffer, size);
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

int eventfd(unsigned int initial, int flags)
{
	const int descriptor = open_fifo(flags);
	if (descriptor >= 0 && initial) {
		const uint64_t value = initial;
		if (write(descriptor, &value, sizeof value) != sizeof value)
			return fail(descriptor);
	}
	return descriptor;
}

int inotify_init1(int flags)
{
	return open_fifo(flags | O_NONBLOCK);
}

int inotify_add_watch(int descriptor, const char *path, uint32_t)
{
	const int directory = open(path, O_EVTONLY | O_CLOEXEC);
	if (directory < 0)
		return -1;
	auto *watch = new DirectoryWatch{descriptor, directory, nullptr};
	watch->source = dispatch_source_create(DISPATCH_SOURCE_TYPE_VNODE, uintptr_t(directory),
		DISPATCH_VNODE_WRITE | DISPATCH_VNODE_DELETE | DISPATCH_VNODE_RENAME, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
	if (!watch->source) {
		close(directory);
		delete watch;
		errno = ENOMEM;
		return -1;
	}
	dispatch_set_context(watch->source, watch);
	dispatch_source_set_event_handler_f(watch->source, directory_changed);
	dispatch_activate(watch->source);
	// The watch lives as long as the process: the runtime watches one directory and never removes it.
	return directory;
}

int inotify_rm_watch(int, int)
{
	return 0;
}

long raven_darwin_futex(void *word, int operation, uint32_t value)
{
	switch (operation & ~FUTEX_PRIVATE_FLAG) {
	case FUTEX_WAIT:
		if (os_sync_wait_on_address(word, value, sizeof(uint32_t), OS_SYNC_WAIT_ON_ADDRESS_SHARED) < 0)
			return errno == EFAULT ? -1 : 0;
		return 0;
	case FUTEX_WAKE:
		if (os_sync_wake_by_address_all(word, sizeof(uint32_t), OS_SYNC_WAKE_BY_ADDRESS_SHARED) < 0 && errno != ENOENT)
			return -1;
		return 0;
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
	const int descriptor = (socket)(domain, type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK), protocol);
	if (descriptor < 0)
		return -1;
	if (set_flags(descriptor, type & SOCK_CLOEXEC, type & SOCK_NONBLOCK) != 0)
		return fail(descriptor);
	return no_sigpipe(descriptor);
}

int raven_darwin_socketpair(int domain, int type, int protocol, int descriptors[2])
{
	if ((socketpair)(domain, type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK), protocol, descriptors) != 0)
		return -1;
	for (int i = 0; i < 2; i++) {
		if (set_flags(descriptors[i], type & SOCK_CLOEXEC, type & SOCK_NONBLOCK) != 0) {
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
	if (set_flags(accepted, flags & SOCK_CLOEXEC, flags & SOCK_NONBLOCK) != 0)
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
