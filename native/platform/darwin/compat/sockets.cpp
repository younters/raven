// Linux Unix-socket behaviour the runtime relies on, on macOS: SOCK_SEQPACKET and abstract socket names.
//
// Sequenced packets. macOS has stream and datagram Unix sockets only, and datagrams have no connections to listen for. A
// SOCK_SEQPACKET socket is a stream socket whose every send is one frame: a 32-bit length, then the message, with any descriptors
// attached to the frame's first byte. The send buffer's low-water mark marks such a socket, so any process that holds it, including
// one that received it through SCM_RIGHTS or was spawned with it, frames its messages the same way; the same mark makes every frame
// atomic: the kernel takes a stream write that fits the low-water mark whole or, without blocking, not at all, and never splits one
// writer's frame with another's. A receive takes exactly one frame, truncating it to the buffer as Linux does.
//
// Abstract names. A name whose first byte is zero lives in no file system on Linux and disappears with the socket. Here it is a
// socket file in a per-user runtime directory, one per socket type so that a client of the wrong type is refused as on Linux, and a
// lock file whose open-file-description lock the binding process holds. The lock tells a live name from the file of a process that
// is gone, which the next bind of the name replaces; closing the socket removes both files.
//
// send, recv, sendmsg, recvmsg, bind, connect and close are defined here, so every call the runtime makes reaches these first; the
// system's versions do the work. Other libraries keep calling the system directly.

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

// The send buffer of a framed socket, and its low-water mark: the largest frame, and the value that tells a framed socket apart.
// Linux takes sequenced packets up to about its default send buffer, 208 KiB.
constexpr int frame_buffer = 1 << 20;
constexpr int frame_mark = (256 << 10) + 0x5ec;
constexpr size_t frame_header = sizeof(uint32_t);
constexpr size_t largest_message = size_t(frame_mark) - frame_header;
// MSG_CMSG_CLOEXEC as the shim header defines it: the descriptors a receive takes are made close-on-exec.
constexpr int cloexec_flag = MSG_CMSG_CLOEXEC;

template <typename Function>
Function system_function(const char *name)
{
	return reinterpret_cast<Function>(dlsym(RTLD_NEXT, name));
}

ssize_t system_sendmsg(int descriptor, const msghdr *message, int flags)
{
	static const auto function = system_function<ssize_t (*)(int, const msghdr *, int)>("sendmsg");
	return function(descriptor, message, flags);
}

ssize_t system_recvmsg(int descriptor, msghdr *message, int flags)
{
	static const auto function = system_function<ssize_t (*)(int, msghdr *, int)>("recvmsg");
	return function(descriptor, message, flags);
}

int system_bind(int descriptor, const sockaddr *address, socklen_t length)
{
	static const auto function = system_function<int (*)(int, const sockaddr *, socklen_t)>("bind");
	return function(descriptor, address, length);
}

int system_connect(int descriptor, const sockaddr *address, socklen_t length)
{
	static const auto function = system_function<int (*)(int, const sockaddr *, socklen_t)>("connect");
	return function(descriptor, address, length);
}

int system_close(int descriptor)
{
	static const auto function = system_function<int (*)(int)>("close");
	return function(descriptor);
}

bool framed(int descriptor)
{
	const int saved = errno;
	int mark = 0;
	socklen_t length = sizeof mark;
	const bool result = getsockopt(descriptor, SOL_SOCKET, SO_SNDLOWAT, &mark, &length) == 0 && mark == frame_mark;
	errno = saved;
	return result;
}

// Partial frames are visible only while the sender's kernel call appends the rest; a receive that met one waits this long at most.
bool frame_wait(timespec &deadline)
{
	timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (!deadline.tv_sec && !deadline.tv_nsec) {
		deadline = now;
		deadline.tv_sec += 5;
	} else if (now.tv_sec > deadline.tv_sec || (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
		errno = EIO;
		return false;
	}
	sched_yield();
	return true;
}

// Receivers of one descriptor in this process take frames one at a time.
std::mutex &reader_lock(int descriptor)
{
	static std::mutex locks[64];
	return locks[unsigned(descriptor) % 64];
}

void close_on_exec(const msghdr &message)
{
	if (!message.msg_control || !message.msg_controllen)
		return;
	for (auto *header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
		if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS)
			continue;
		const size_t count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
		for (size_t index = 0; index < count; ++index) {
			int received;
			memcpy(&received, CMSG_DATA(header) + index * sizeof(int), sizeof(int));
			fcntl(received, F_SETFD, FD_CLOEXEC);
		}
	}
}

size_t capacity_of(const msghdr &message)
{
	size_t capacity = 0;
	for (int index = 0; index < message.msg_iovlen; ++index)
		capacity += message.msg_iov[index].iov_len;
	return capacity;
}

ssize_t frame_send(int descriptor, const msghdr *message, int flags)
{
	const size_t length = capacity_of(*message);
	if (length > largest_message) {
		errno = EMSGSIZE;
		return -1;
	}
	uint32_t header = uint32_t(length);
	std::vector<iovec> vectors;
	std::vector<uint8_t> joined;
	if (message->msg_iovlen + 1 <= IOV_MAX) {
		vectors.reserve(size_t(message->msg_iovlen) + 1);
		vectors.push_back({&header, frame_header});
		for (int index = 0; index < message->msg_iovlen; ++index)
			vectors.push_back(message->msg_iov[index]);
	} else {
		joined.resize(frame_header + length);
		memcpy(joined.data(), &header, frame_header);
		size_t at = frame_header;
		for (int index = 0; index < message->msg_iovlen; ++index) {
			memcpy(joined.data() + at, message->msg_iov[index].iov_base, message->msg_iov[index].iov_len);
			at += message->msg_iov[index].iov_len;
		}
		vectors.push_back({joined.data(), joined.size()});
	}
	msghdr frame{};
	frame.msg_iov = vectors.data();
	frame.msg_iovlen = int(vectors.size());
	frame.msg_control = message->msg_control;
	frame.msg_controllen = message->msg_controllen;
	const ssize_t sent = system_sendmsg(descriptor, &frame, flags);
	if (sent < 0)
		return -1;
	// The kernel takes a frame within the low-water mark whole; should it ever stop short, the rest follows before anything else.
	size_t done = size_t(sent);
	const size_t whole = frame_header + length;
	while (done < whole) {
		size_t skip = done;
		size_t index = 0;
		while (skip >= vectors[index].iov_len) {
			skip -= vectors[index].iov_len;
			++index;
		}
		iovec rest{static_cast<uint8_t *>(vectors[index].iov_base) + skip, vectors[index].iov_len - skip};
		msghdr more{};
		more.msg_iov = &rest;
		more.msg_iovlen = 1;
		const ssize_t count = system_sendmsg(descriptor, &more, flags & ~MSG_DONTWAIT);
		if (count < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				pollfd entry{descriptor, POLLOUT, 0};
				poll(&entry, 1, 100);
				continue;
			}
			return -1;
		}
		done += size_t(count);
	}
	return ssize_t(length);
}

// The bytes at the head of the stream, without taking them: the count, 0 at the end, or -1 with errno.
ssize_t peek(int descriptor, void *buffer, size_t size, int flags)
{
	return recvfrom(descriptor, buffer, size, MSG_PEEK | flags, nullptr, nullptr);
}

ssize_t frame_peek(int descriptor, msghdr *message, int flags)
{
	const size_t capacity = capacity_of(*message);
	std::vector<uint8_t> buffer(frame_header + capacity);
	timespec deadline{};
	ssize_t count;
	for (;;) {
		count = peek(descriptor, buffer.data(), buffer.size(), flags & MSG_DONTWAIT);
		if (count <= 0)
			return count;
		if (size_t(count) >= frame_header)
			break;
		if (!frame_wait(deadline))
			return -1;
	}
	// A peek reports what has arrived of the frame so far, which is all of it but for a frame still being appended.
	uint32_t length;
	memcpy(&length, buffer.data(), frame_header);
	size_t copied = 0, left = std::min<size_t>(length, size_t(count) - frame_header);
	for (int index = 0; index < message->msg_iovlen && left; ++index) {
		const size_t part = std::min(left, message->msg_iov[index].iov_len);
		memcpy(message->msg_iov[index].iov_base, buffer.data() + frame_header + copied, part);
		copied += part;
		left -= part;
	}
	message->msg_controllen = 0;
	message->msg_flags = length > capacity ? MSG_TRUNC : 0;
	return ssize_t(copied);
}

ssize_t frame_receive(int descriptor, msghdr *message, int flags)
{
	if (message->msg_name)
		message->msg_namelen = 0;
	if (flags & MSG_PEEK)
		return frame_peek(descriptor, message, flags);
	const bool cloexec = flags & cloexec_flag;
	const int passed = flags & ~(cloexec_flag | MSG_DONTWAIT | MSG_WAITALL | MSG_TRUNC);
	for (;;) {
		// Wait for a frame the way the caller asked to, outside the lock.
		uint32_t header;
		ssize_t count = peek(descriptor, &header, frame_header, flags & MSG_DONTWAIT);
		if (count <= 0) {
			if (count == 0)
				message->msg_controllen = 0, message->msg_flags = 0;
			return count;
		}
		std::lock_guard guard(reader_lock(descriptor));
		count = peek(descriptor, &header, frame_header, MSG_DONTWAIT);
		if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			continue;
		if (count <= 0) {
			if (count == 0)
				message->msg_controllen = 0, message->msg_flags = 0;
			return count;
		}
		timespec deadline{};
		while (count < ssize_t(frame_header)) {
			if (count == 0) {
				message->msg_controllen = 0, message->msg_flags = 0;
				return 0;
			}
			if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
				return -1;
			if (!frame_wait(deadline))
				return -1;
			count = peek(descriptor, &header, frame_header, MSG_DONTWAIT);
		}
		if (header > largest_message) {
			errno = EBADMSG;
			return -1;
		}
		// The header carries the descriptors sent with the frame.
		iovec vector{&header, frame_header};
		msghdr head{};
		head.msg_iov = &vector;
		head.msg_iovlen = 1;
		head.msg_control = message->msg_control;
		head.msg_controllen = message->msg_controllen;
		do count = system_recvmsg(descriptor, &head, passed | MSG_DONTWAIT); while (count < 0 && errno == EINTR);
		if (count != ssize_t(frame_header)) {
			if (count >= 0)
				errno = EIO;
			return -1;
		}
		message->msg_controllen = head.msg_controllen;
		message->msg_flags = head.msg_flags & MSG_CTRUNC;
		if (cloexec)
			close_on_exec(*message);
		// The message goes into the caller's buffers; what does not fit them is read and dropped.
		size_t left = header, copied = 0;
		int index = 0;
		size_t offset = 0;
		uint8_t scratch[4096];
		while (left) {
			while (index < message->msg_iovlen && offset == message->msg_iov[index].iov_len) {
				++index;
				offset = 0;
			}
			void *target;
			size_t size;
			if (index < message->msg_iovlen) {
				target = static_cast<uint8_t *>(message->msg_iov[index].iov_base) + offset;
				size = std::min(left, message->msg_iov[index].iov_len - offset);
			} else {
				target = scratch;
				size = std::min(left, sizeof scratch);
			}
			count = recvfrom(descriptor, target, size, passed | MSG_DONTWAIT, nullptr, nullptr);
			if (count > 0) {
				left -= size_t(count);
				if (index < message->msg_iovlen) {
					offset += size_t(count);
					copied += size_t(count);
				}
				continue;
			}
			if (count == 0)
				break;
			if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
				return -1;
			if (!frame_wait(deadline))
				return -1;
		}
		if (header > copied)
			message->msg_flags |= MSG_TRUNC;
		return ssize_t(copied);
	}
}

// Abstract names.

struct Binding {
	int descriptor;
	pid_t owner;
	int lock;
	std::string socket, lock_path;
};

struct Bindings {
	std::mutex mutex;
	std::vector<Binding> list;
	std::atomic<size_t> count{0};
	bool swept = false, exit_registered = false;
};

// Never destroyed: sockets are closed by static destructors after the others have run.
Bindings &bindings()
{
	static auto *state = new Bindings;
	return *state;
}

const std::string &runtime_directory()
{
	static const std::string directory = [] {
		char base[PATH_MAX];
		std::string path;
		if (confstr(_CS_DARWIN_USER_TEMP_DIR, base, sizeof base) > 0)
			path = base;
		else {
			const char *temporary = getenv("TMPDIR");
			path = temporary && *temporary ? temporary : "/tmp";
		}
		if (path.back() != '/')
			path += '/';
		path += "raven-" + std::to_string(geteuid()) + "/";
		mkdir(path.c_str(), 0700);
		struct stat status;
		// Somebody else's directory, or anything but a directory, is no place for sockets this user's processes trust.
		if (lstat(path.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) || status.st_uid != geteuid() || (status.st_mode & 077))
			return std::string();
		return path;
	}();
	return directory;
}

// The files behind an abstract name: socket files for stream and sequenced-packet sockets, and the lock.
struct Names {
	std::string stream, packet, lock;
};

std::optional<Names> names_of(const char *name, size_t length)
{
	const auto &directory = runtime_directory();
	if (directory.empty()) {
		errno = EACCES;
		return std::nullopt;
	}
	std::string base;
	for (size_t index = 0; index < length; ++index) {
		const unsigned char character = static_cast<unsigned char>(name[index]);
		if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') ||
			character == '.' || character == '_' || character == '-')
			base += char(character);
		else {
			static const char digits[] = "0123456789abcdef";
			base += '%';
			base += digits[character >> 4];
			base += digits[character & 15];
		}
	}
	// A path that does not fit sun_path is named by a hash of the name instead.
	if (directory.size() + base.size() + 3 > sizeof(sockaddr_un::sun_path)) {
		uint64_t hash = 14695981039346656037ULL;
		for (size_t index = 0; index < length; ++index) {
			hash ^= static_cast<unsigned char>(name[index]);
			hash *= 1099511628211ULL;
		}
		char text[24];
		snprintf(text, sizeof text, "#%016llx", static_cast<unsigned long long>(hash));
		base = text;
	}
	const auto prefix = directory + base;
	return Names{prefix + ".s", prefix + ".p", prefix + ".lock"};
}

bool abstract(const sockaddr *address, socklen_t length)
{
	return address && address->sa_family == AF_UNIX && length > offsetof(sockaddr_un, sun_path) &&
		!reinterpret_cast<const sockaddr_un *>(address)->sun_path[0];
}

// The files of an abstract address, or nothing with errno; an empty name, which Linux binds to one it makes up, is not supported.
std::optional<Names> abstract_names(const sockaddr *address, socklen_t length)
{
	if (length <= offsetof(sockaddr_un, sun_path) + 1) {
		errno = EINVAL;
		return std::nullopt;
	}
	const size_t size = std::min<size_t>(length, sizeof(sockaddr_un)) - offsetof(sockaddr_un, sun_path) - 1;
	return names_of(reinterpret_cast<const sockaddr_un *>(address)->sun_path + 1, size);
}

bool fill_address(const std::string &path, sockaddr_un &address)
{
	memset(&address, 0, sizeof address);
	address.sun_family = AF_UNIX;
	if (path.size() >= sizeof address.sun_path) {
		errno = ENAMETOOLONG;
		return false;
	}
	memcpy(address.sun_path, path.c_str(), path.size() + 1);
	address.sun_len = uint8_t(SUN_LEN(&address));
	return true;
}

// Whether the descriptor is still the socket bound to the path: one closed past close(), and its number reused, is not.
bool still_bound(const Binding &binding)
{
	sockaddr_un address{};
	socklen_t length = sizeof address;
	if (getsockname(binding.descriptor, reinterpret_cast<sockaddr *>(&address), &length) != 0)
		return false;
	return strncmp(address.sun_path, binding.socket.c_str(), sizeof address.sun_path) == 0;
}

void release(const Binding &binding)
{
	if (binding.owner == getpid()) {
		unlink(binding.socket.c_str());
		unlink(binding.lock_path.c_str());
	}
	system_close(binding.lock);
}

// The lock of a name, held through an open file description of its own; -1 with EADDRINUSE while another one holds it. A bind
// creates the file and waits out the moment another process's sweep holds the lock of a name that has none; a sweep does neither.
int acquire(const std::string &path, bool binding)
{
	for (int attempt = 0, busy = 0; attempt < 8; ++attempt) {
		const int descriptor = open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | (binding ? O_CREAT : 0), 0600);
		if (descriptor < 0)
			return -1;
		struct flock lock{};
		lock.l_type = F_WRLCK;
		lock.l_whence = SEEK_SET;
		if (fcntl(descriptor, F_OFD_SETLK, &lock) != 0) {
			const int error = errno;
			system_close(descriptor);
			if (binding && (error == EAGAIN || error == EACCES) && ++busy < 4) {
				const timespec pause{0, 1000000};
				nanosleep(&pause, nullptr);
				--attempt;
				continue;
			}
			errno = error == EAGAIN || error == EACCES ? EADDRINUSE : error;
			return -1;
		}
		// The holder before may have removed the file between our open and our lock: only the lock on the file the name has counts.
		struct stat opened, named;
		if (fstat(descriptor, &opened) == 0 && stat(path.c_str(), &named) == 0 && opened.st_dev == named.st_dev && opened.st_ino == named.st_ino)
			return descriptor;
		system_close(descriptor);
	}
	errno = EADDRINUSE;
	return -1;
}

bool held(const std::string &path)
{
	const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (descriptor < 0)
		return false;
	struct flock lock{};
	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	const bool result = fcntl(descriptor, F_OFD_GETLK, &lock) == 0 && lock.l_type != F_UNLCK;
	system_close(descriptor);
	return result;
}

void release_all()
{
	auto &state = bindings();
	std::lock_guard guard(state.mutex);
	for (const auto &binding : state.list)
		release(binding);
	state.list.clear();
	state.count = 0;
}

// The files of names whose processes ended without closing them, removed under their own lock so that a bind cannot race it.
void sweep(Bindings &state)
{
	if (state.swept)
		return;
	state.swept = true;
	const auto &directory = runtime_directory();
	DIR *listing = opendir(directory.c_str());
	if (!listing)
		return;
	std::vector<std::string> locks;
	while (const auto *entry = readdir(listing)) {
		const std::string name = entry->d_name;
		if (name.size() > 5 && name.compare(name.size() - 5, 5, ".lock") == 0)
			locks.push_back(directory + name);
	}
	closedir(listing);
	for (const auto &path : locks) {
		const int lock = acquire(path, false);
		if (lock < 0)
			continue;
		const auto base = path.substr(0, path.size() - 5);
		unlink((base + ".s").c_str());
		unlink((base + ".p").c_str());
		unlink(path.c_str());
		system_close(lock);
	}
}

void forget(int descriptor)
{
	auto &state = bindings();
	std::lock_guard guard(state.mutex);
	for (auto entry = state.list.begin(); entry != state.list.end(); ++entry)
		if (entry->descriptor == descriptor) {
			if (still_bound(*entry))
				release(*entry);
			else
				system_close(entry->lock);
			state.list.erase(entry);
			--state.count;
			return;
		}
}

int bind_abstract(int descriptor, const Names &names)
{
	auto &state = bindings();
	std::lock_guard guard(state.mutex);
	sweep(state);
	for (auto entry = state.list.begin(); entry != state.list.end(); ++entry)
		if (entry->lock_path == names.lock) {
			if (entry->owner == getpid() && still_bound(*entry)) {
				errno = EADDRINUSE;
				return -1;
			}
			release(*entry);
			state.list.erase(entry);
			--state.count;
			break;
		}
	const int lock = acquire(names.lock, true);
	if (lock < 0)
		return -1;
	// Whatever socket files the name still has belong to a process that is gone.
	unlink(names.stream.c_str());
	unlink(names.packet.c_str());
	const auto &path = framed(descriptor) ? names.packet : names.stream;
	sockaddr_un address;
	if (!fill_address(path, address) || system_bind(descriptor, reinterpret_cast<const sockaddr *>(&address), socklen_t(SUN_LEN(&address))) != 0) {
		const int error = errno;
		unlink(names.lock.c_str());
		system_close(lock);
		errno = error;
		return -1;
	}
	state.list.push_back({descriptor, getpid(), lock, path, names.lock});
	++state.count;
	if (!state.exit_registered) {
		state.exit_registered = true;
		atexit(release_all);
	}
	return 0;
}

void apply_frame(int descriptor)
{
	setsockopt(descriptor, SOL_SOCKET, SO_SNDBUF, &frame_buffer, sizeof frame_buffer);
	setsockopt(descriptor, SOL_SOCKET, SO_SNDLOWAT, &frame_mark, sizeof frame_mark);
}

}

extern "C" {

int raven_darwin_frame_socket(int descriptor)
{
	apply_frame(descriptor);
	if (!framed(descriptor)) {
		errno = ENOBUFS;
		return -1;
	}
	return 0;
}

int raven_darwin_framed_socket(int descriptor)
{
	return framed(descriptor);
}

int raven_darwin_unix_name_bound(const char *name, size_t length)
{
	const auto names = names_of(name, length);
	if (!names)
		return 0;
	{
		auto &state = bindings();
		std::lock_guard guard(state.mutex);
		for (const auto &binding : state.list)
			if (binding.lock_path == names->lock && binding.owner == getpid() && still_bound(binding))
				return 1;
	}
	return held(names->lock);
}

ssize_t sendmsg(int descriptor, const struct msghdr *message, int flags)
{
	if (framed(descriptor))
		return frame_send(descriptor, message, flags);
	return system_sendmsg(descriptor, message, flags);
}

ssize_t send(int descriptor, const void *buffer, size_t length, int flags)
{
	if (framed(descriptor)) {
		iovec vector{const_cast<void *>(buffer), length};
		msghdr message{};
		message.msg_iov = &vector;
		message.msg_iovlen = 1;
		return frame_send(descriptor, &message, flags);
	}
	return sendto(descriptor, buffer, length, flags, nullptr, 0);
}

ssize_t recvmsg(int descriptor, struct msghdr *message, int flags)
{
	if (framed(descriptor))
		return frame_receive(descriptor, message, flags);
	const ssize_t count = system_recvmsg(descriptor, message, flags & ~cloexec_flag);
	if (count >= 0 && (flags & cloexec_flag))
		close_on_exec(*message);
	return count;
}

ssize_t recv(int descriptor, void *buffer, size_t length, int flags)
{
	if (framed(descriptor)) {
		iovec vector{buffer, length};
		msghdr message{};
		message.msg_iov = &vector;
		message.msg_iovlen = 1;
		return frame_receive(descriptor, &message, flags);
	}
	return recvfrom(descriptor, buffer, length, flags & ~cloexec_flag, nullptr, nullptr);
}

int bind(int descriptor, const struct sockaddr *address, socklen_t length)
{
	if (!abstract(address, length))
		return system_bind(descriptor, address, length);
	const auto names = abstract_names(address, length);
	return names ? bind_abstract(descriptor, *names) : -1;
}

int connect(int descriptor, const struct sockaddr *address, socklen_t length)
{
	if (!abstract(address, length))
		return system_connect(descriptor, address, length);
	const auto names = abstract_names(address, length);
	if (!names)
		return -1;
	sockaddr_un target;
	if (!fill_address(framed(descriptor) ? names->packet : names->stream, target))
		return -1;
	const int result = system_connect(descriptor, reinterpret_cast<const sockaddr *>(&target), socklen_t(SUN_LEN(&target)));
	// An abstract name nobody binds is refused, not missing.
	if (result != 0 && errno == ENOENT)
		errno = ECONNREFUSED;
	return result;
}

int close(int descriptor)
{
	if (bindings().count.load(std::memory_order_relaxed))
		forget(descriptor);
	return system_close(descriptor);
}

}
