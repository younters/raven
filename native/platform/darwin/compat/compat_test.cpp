// The Linux interprocess behaviour the macOS shims provide (sockets.cpp, procfs.cpp): sequenced packets, abstract socket names,
// the /proc process files, SO_ACCEPTCONN, and reopening a file as a new open file description.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern char **environ;

namespace {

void check(bool condition, const std::string &message)
{
	if (!condition)
		throw std::runtime_error(message + (errno ? std::string(" (") + std::strerror(errno) + ")" : std::string()));
}

std::string self_path()
{
	char path[4096];
	const auto length = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
	check(length > 0, "cannot read /proc/self/exe");
	return std::string(path, size_t(length));
}

pid_t spawn(const std::vector<std::string> &arguments)
{
	std::vector<char *> argv;
	auto copies = arguments;
	for (auto &argument : copies)
		argv.push_back(argument.data());
	argv.push_back(nullptr);
	pid_t pid = 0;
	errno = ::posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), environ);
	check(errno == 0, "cannot spawn " + arguments[0]);
	return pid;
}

int exit_status(pid_t pid)
{
	int status = 0;
	check(::waitpid(pid, &status, 0) == pid, "cannot wait for child");
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

sockaddr_un abstract_address(const std::string &name, socklen_t &length)
{
	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	const size_t size = std::min(name.size(), sizeof(address.sun_path) - 1);
	std::memcpy(address.sun_path + 1, name.data(), size);
	length = socklen_t(offsetof(sockaddr_un, sun_path) + 1 + size);
	return address;
}

std::string message(int descriptor, size_t capacity = 512 << 10, int flags = 0)
{
	std::string buffer(capacity, '\0');
	const auto count = ::recv(descriptor, buffer.data(), buffer.size(), flags);
	check(count >= 0, "cannot receive a message");
	buffer.resize(size_t(count));
	return buffer;
}

void send_text(int descriptor, const std::string &text)
{
	check(::send(descriptor, text.data(), text.size(), MSG_NOSIGNAL) == ssize_t(text.size()), "cannot send a message");
}

void packets()
{
	int pair[2];
	check(!::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, pair), "cannot create a sequenced-packet pair");
	char byte;
	check(::recv(pair[1], &byte, 1, 0) < 0 && errno == EAGAIN, "an empty pair is not readable");
	errno = 0;
	send_text(pair[0], "abc");
	send_text(pair[0], "");
	send_text(pair[0], "defgh");
	check(::recv(pair[1], &byte, 1, MSG_PEEK) == 1 && byte == 'a', "a peek sees the first message");
	check(message(pair[1]) == "abc", "messages keep their boundaries");
	check(message(pair[1]).empty(), "an empty message arrives empty");
	check(message(pair[1]) == "defgh", "an empty message is not the end");
	// A message longer than the buffer is cut, and the rest is gone.
	send_text(pair[0], "0123456789");
	send_text(pair[0], "next");
	char part[4];
	iovec vector{part, sizeof part};
	msghdr header{};
	header.msg_iov = &vector;
	header.msg_iovlen = 1;
	check(::recvmsg(pair[1], &header, 0) == 4 && std::memcmp(part, "0123", 4) == 0 && (header.msg_flags & MSG_TRUNC), "a long message is truncated");
	check(message(pair[1]) == "next", "the rest of a truncated message is dropped");
	// One sendmsg is one message, whatever its vectors.
	char first[] = "first-", second[] = "second";
	iovec vectors[2] = {{first, 6}, {second, 6}};
	msghdr gathered{};
	gathered.msg_iov = vectors;
	gathered.msg_iovlen = 2;
	check(::sendmsg(pair[0], &gathered, MSG_NOSIGNAL) == 12 && message(pair[1]) == "first-second", "gathered vectors make one message");
	// Descriptors travel with their message and arrive close-on-exec when asked.
	int pipe_ends[2];
	check(!::pipe(pipe_ends), "cannot create a pipe");
	char payload[] = "fd";
	iovec carried{payload, 2};
	alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
	msghdr outgoing{};
	outgoing.msg_iov = &carried;
	outgoing.msg_iovlen = 1;
	outgoing.msg_control = control;
	outgoing.msg_controllen = sizeof control;
	auto *rights = CMSG_FIRSTHDR(&outgoing);
	rights->cmsg_level = SOL_SOCKET;
	rights->cmsg_type = SCM_RIGHTS;
	rights->cmsg_len = CMSG_LEN(sizeof(int));
	std::memcpy(CMSG_DATA(rights), &pipe_ends[1], sizeof(int));
	check(::sendmsg(pair[0], &outgoing, MSG_NOSIGNAL) == 2, "cannot send a descriptor");
	send_text(pair[0], "after");
	char incoming_bytes[16];
	iovec incoming_vector{incoming_bytes, sizeof incoming_bytes};
	alignas(cmsghdr) char incoming_control[CMSG_SPACE(sizeof(int) * 4)] = {};
	msghdr incoming{};
	incoming.msg_iov = &incoming_vector;
	incoming.msg_iovlen = 1;
	incoming.msg_control = incoming_control;
	incoming.msg_controllen = sizeof incoming_control;
	check(::recvmsg(pair[1], &incoming, MSG_DONTWAIT | MSG_CMSG_CLOEXEC) == 2, "cannot receive a descriptor");
	const auto *received_header = CMSG_FIRSTHDR(&incoming);
	check(received_header && received_header->cmsg_type == SCM_RIGHTS, "the descriptor did not arrive");
	int received;
	std::memcpy(&received, CMSG_DATA(received_header), sizeof(int));
	check((::fcntl(received, F_GETFD) & FD_CLOEXEC) && ::write(received, "x", 1) == 1 && ::read(pipe_ends[0], &byte, 1) == 1 && byte == 'x',
		"the received descriptor is close-on-exec and works");
	::close(received);
	check(message(pair[1]) == "after", "the message after a descriptor is its own");
	// Large messages, and one past the largest.
	const std::string large(200 << 10, 'L');
	check(::send(pair[0], large.data(), large.size(), MSG_NOSIGNAL) == ssize_t(large.size()) && message(pair[1]) == large, "a large message arrives whole");
	const std::string huge(300 << 10, 'H');
	check(::send(pair[0], huge.data(), huge.size(), MSG_NOSIGNAL) < 0 && errno == EMSGSIZE, "an oversized message is refused");
	errno = 0;
	// Writers on several threads never interleave their messages.
	std::atomic<bool> failed{false};
	std::vector<std::thread> writers;
	constexpr int per_writer = 300;
	for (int writer = 0; writer < 4; ++writer)
		writers.emplace_back([&, writer] {
			for (int index = 0; index < per_writer; ++index) {
				const std::string text(size_t(1 + (index * 37 + writer * 101) % 9000), char('A' + writer));
				for (;;) {
					const auto sent = ::send(pair[0], text.data(), text.size(), MSG_NOSIGNAL);
					if (sent == ssize_t(text.size()))
						break;
					if (sent >= 0 || errno != EAGAIN) {
						failed = true;
						return;
					}
					pollfd entry{pair[0], POLLOUT, 0};
					::poll(&entry, 1, 100);
				}
			}
		});
	int counts[4] = {};
	for (int total = 0; total < 4 * per_writer;) {
		std::string buffer(16 << 10, '\0');
		const auto count = ::recv(pair[1], buffer.data(), buffer.size(), 0);
		if (count < 0 && errno == EAGAIN) {
			pollfd entry{pair[1], POLLIN, 0};
			::poll(&entry, 1, 100);
			continue;
		}
		check(count > 0, "concurrent messages stopped");
		const int writer = buffer[0] - 'A';
		check(writer >= 0 && writer < 4 && size_t(count) == size_t(1 + (counts[writer] * 37 + writer * 101) % 9000) &&
			buffer.find_first_not_of(buffer[0]) >= size_t(count), "concurrent messages interleaved");
		++counts[writer];
		++total;
	}
	for (auto &thread : writers)
		thread.join();
	check(!failed, "a concurrent writer failed");
	// The end of the peer reads as the end, peeking or not.
	::close(pair[0]);
	check(::recv(pair[1], &byte, 1, MSG_PEEK | MSG_DONTWAIT) == 0 && ::recv(pair[1], &byte, 1, 0) == 0, "a closed peer reads as the end");
	::close(pair[1]);
	::close(pipe_ends[0]);
	::close(pipe_ends[1]);
}

// A child started with one end of a pair, as raven passes --handle-channel-fd: it frames its messages as well.
int packet_child(int descriptor)
{
	char buffer[64];
	iovec vector{buffer, sizeof buffer};
	alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
	msghdr header{};
	header.msg_iov = &vector;
	header.msg_iovlen = 1;
	header.msg_control = control;
	header.msg_controllen = sizeof control;
	if (::recvmsg(descriptor, &header, MSG_CMSG_CLOEXEC) != 4 || std::memcmp(buffer, "ping", 4) != 0 || !CMSG_FIRSTHDR(&header))
		return 2;
	int pipe_end;
	std::memcpy(&pipe_end, CMSG_DATA(CMSG_FIRSTHDR(&header)), sizeof(int));
	if (::write(pipe_end, "y", 1) != 1)
		return 3;
	if (::send(descriptor, "pong", 4, 0) != 4 || ::send(descriptor, "again", 5, 0) != 5)
		return 4;
	return 0;
}

void packets_across_spawn()
{
	int pair[2];
	check(!::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair), "cannot create a pair for a child");
	::fcntl(pair[0], F_SETFD, FD_CLOEXEC);
	const pid_t child = spawn({self_path(), "--packet-child=" + std::to_string(pair[1])});
	::close(pair[1]);
	int pipe_ends[2];
	check(!::pipe(pipe_ends), "cannot create a pipe for a child");
	char payload[] = "ping";
	iovec vector{payload, 4};
	alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
	msghdr header{};
	header.msg_iov = &vector;
	header.msg_iovlen = 1;
	header.msg_control = control;
	header.msg_controllen = sizeof control;
	auto *rights = CMSG_FIRSTHDR(&header);
	rights->cmsg_level = SOL_SOCKET;
	rights->cmsg_type = SCM_RIGHTS;
	rights->cmsg_len = CMSG_LEN(sizeof(int));
	std::memcpy(CMSG_DATA(rights), &pipe_ends[1], sizeof(int));
	check(::sendmsg(pair[0], &header, 0) == 4, "cannot send to the child");
	check(message(pair[0]) == "pong" && message(pair[0]) == "again", "the child's messages keep their boundaries");
	char byte;
	check(::read(pipe_ends[0], &byte, 1) == 1 && byte == 'y', "the child used the descriptor it was sent");
	check(exit_status(child) == 0, "the packet child failed");
	check(message(pair[0]).empty(), "the child's end closes with it");
	::close(pair[0]);
	::close(pipe_ends[0]);
	::close(pipe_ends[1]);
}

int bind_child(const std::string &name)
{
	const int listener = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
	socklen_t length;
	const auto address = abstract_address(name, length);
	if (listener < 0 || ::bind(listener, reinterpret_cast<const sockaddr *>(&address), length) || ::listen(listener, 4))
		return 2;
	// Gone without closing the socket, as a crash would leave it.
	_exit(raven_darwin_unix_name_bound(name.data(), name.size()) ? 0 : 3);
}

void abstract_names()
{
	const std::string name = "raven-compat-test-" + std::to_string(::getpid());
	socklen_t length;
	const auto address = abstract_address(name, length);
	check(!raven_darwin_unix_name_bound(name.data(), name.size()), "an unused name is not bound");
	const int listener = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	check(listener >= 0 && !::bind(listener, reinterpret_cast<const sockaddr *>(&address), length) && !::listen(listener, 8), "cannot listen on an abstract name");
	check(raven_darwin_unix_name_bound(name.data(), name.size()), "a listening name is bound");
	int listening = 0;
	socklen_t size = sizeof listening;
	check(!::getsockopt(listener, SOL_SOCKET, SO_ACCEPTCONN, &listening, &size) && listening == 1, "SO_ACCEPTCONN reports a listener");
	for (const int type : {SOCK_SEQPACKET, SOCK_STREAM}) {
		const int other = ::socket(AF_UNIX, type, 0);
		check(::bind(other, reinterpret_cast<const sockaddr *>(&address), length) < 0 && errno == EADDRINUSE, "a bound name is in use for any type");
		::close(other);
	}
	errno = 0;
	// A stream client is refused by a sequenced-packet listener, as on Linux; a client of the same type connects.
	const int stream = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	check(::connect(stream, reinterpret_cast<const sockaddr *>(&address), length) < 0 && errno == ECONNREFUSED, "a stream client is refused");
	::close(stream);
	errno = 0;
	const int client = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	check(!::connect(client, reinterpret_cast<const sockaddr *>(&address), length), "cannot connect to an abstract name");
	const int accepted = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
	check(accepted >= 0, "cannot accept a connection");
	check(!::getsockopt(accepted, SOL_SOCKET, SO_ACCEPTCONN, &listening, &size) && listening == 0, "SO_ACCEPTCONN reports a connection");
	ucred peer{};
	socklen_t peer_size = sizeof peer;
	check(!::getsockopt(accepted, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) && peer.pid == ::getpid(), "the peer is this process");
	send_text(client, "one");
	send_text(client, "two");
	check(message(accepted) == "one" && message(accepted) == "two", "a connection carries messages");
	send_text(accepted, "back");
	pollfd entry{client, POLLIN, 0};
	check(::poll(&entry, 1, 1000) == 1 && message(client) == "back", "an accepted connection carries messages");
	::close(accepted);
	::close(client);
	// Closing the listener frees the name.
	::close(listener);
	check(!raven_darwin_unix_name_bound(name.data(), name.size()), "a closed name is not bound");
	const int unbound = ::socket(AF_UNIX, SOCK_SEQPACKET, 0);
	check(::connect(unbound, reinterpret_cast<const sockaddr *>(&address), length) < 0 && errno == ECONNREFUSED, "a free name refuses");
	::close(unbound);
	errno = 0;
	// The name a process left behind when it ended is free again.
	check(exit_status(spawn({self_path(), "--bind-child=" + name})) == 0, "the bind child failed");
	check(!raven_darwin_unix_name_bound(name.data(), name.size()), "an ended process does not keep its name");
	const int again = ::socket(AF_UNIX, SOCK_STREAM, 0);
	check(!::bind(again, reinterpret_cast<const sockaddr *>(&address), length), "cannot bind a name an ended process left");
	::close(again);
	// A name longer than macOS socket paths allow.
	const std::string long_name(180, 'n');
	socklen_t long_length;
	const auto long_address = abstract_address(long_name, long_length);
	const int long_listener = ::socket(AF_UNIX, SOCK_STREAM, 0), long_client = ::socket(AF_UNIX, SOCK_STREAM, 0);
	check(!::bind(long_listener, reinterpret_cast<const sockaddr *>(&long_address), long_length) && !::listen(long_listener, 1) &&
		!::connect(long_client, reinterpret_cast<const sockaddr *>(&long_address), long_length), "a long abstract name works");
	::close(long_client);
	::close(long_listener);
}

void process_files()
{
	std::string line;
	std::getline(raven_darwin::proc_stream("/proc/self/stat"), line);
	const auto close = line.rfind(')');
	check(line.starts_with(std::to_string(::getpid()) + " (") && close != line.npos, "/proc/self/stat names this process: " + line);
	int parent = 0;
	char state = 0;
	check(std::sscanf(line.c_str() + close + 2, "%c %d", &state, &parent) == 2 && parent == ::getppid() && state != 'Z', "/proc/self/stat has the parent");
	// A child: its parent, its arguments, its program, its listing, and its end.
	const auto self = self_path();
	const pid_t child = spawn({self, "--sleep-child", "second argument"});
	std::getline(raven_darwin::proc_stream("/proc/" + std::to_string(child) + "/stat"), line);
	check(std::sscanf(line.c_str() + line.rfind(')') + 2, "%c %d", &state, &parent) == 2 && parent == ::getpid(), "a child's stat has this parent");
	std::string arguments, argument;
	auto command_line = raven_darwin::proc_stream("/proc/" + std::to_string(child) + "/cmdline");
	std::vector<std::string> words;
	while (std::getline(command_line, argument, '\0'))
		words.push_back(argument);
	check(words.size() == 3 && words[1] == "--sleep-child" && words[2] == "second argument", "a child's cmdline has its arguments");
	char program[4096];
	const auto program_length = ::readlink(("/proc/" + std::to_string(child) + "/exe").c_str(), program, sizeof program);
	check(program_length > 0 && std::string(program, size_t(program_length)) == self, "a child's exe is its program");
	std::error_code error;
	bool listed_self = false, listed_child = false;
	for (const auto &entry : raven_darwin::proc_directory(error)) {
		listed_self |= entry.path() == "/proc/" + std::to_string(::getpid());
		listed_child |= entry.path().filename() == std::to_string(child);
	}
	check(!error && listed_self && listed_child, "/proc lists this process and its child");
	::kill(child, SIGKILL);
	for (int attempt = 0; attempt < 200; ++attempt) {
		std::getline(raven_darwin::proc_stream("/proc/" + std::to_string(child) + "/stat"), line);
		if (line.size() > line.rfind(')') + 2 && line[line.rfind(')') + 2] == 'Z')
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	check(line[line.rfind(')') + 2] == 'Z', "an ended child that is not reaped is a zombie: " + line);
	exit_status(child);
	check(raven_darwin::proc_stream("/proc/" + std::to_string(child) + "/stat").str().empty(), "a reaped child has no stat");
	// CPU time in clock ticks, and memory in pages.
	const auto start = std::chrono::steady_clock::now();
	volatile uint64_t spin = 0;
	while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(150))
		spin = spin + 1;
	std::getline(raven_darwin::proc_stream("/proc/self/stat"), line);
	std::istringstream fields(line.substr(line.rfind(')') + 2));
	std::string field;
	uint64_t user = 0;
	for (int index = 3; fields >> field && index <= 14; ++index)
		if (index == 14)
			user = std::stoull(field);
	check(user >= 5, "CPU time is counted in clock ticks: " + line);
	uint64_t pages = 0, resident = 0;
	raven_darwin::proc_stream("/proc/self/statm") >> pages >> resident;
	check(pages > 0 && resident > 0, "/proc/self/statm has sizes");
}

void tcp_table()
{
	const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t length = sizeof address;
	check(!::bind(listener, reinterpret_cast<const sockaddr *>(&address), length) && !::listen(listener, 1) &&
		!::getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length), "cannot listen on TCP");
	char wanted[64];
	std::snprintf(wanted, sizeof wanted, "0100007F:%04X 00000000:0000 0A", unsigned(ntohs(address.sin_port)));
	auto table = raven_darwin::proc_stream("/proc/net/tcp");
	std::string line;
	uint64_t inode = 0;
	while (std::getline(table, line))
		if (line.find(wanted) != line.npos) {
			std::istringstream fields(line);
			std::string skip;
			for (int index = 0; index < 9; ++index)
				fields >> skip;
			fields >> inode;
		}
	check(inode != 0, std::string("/proc/net/tcp lists the listener ") + wanted);
	const auto owners = raven_darwin::socket_owners();
	const auto found = owners.find(inode);
	check(found != owners.end() && found->second == uint32_t(::getpid()), "the listener's owner is this process");
	::close(listener);
	const int listener6 = ::socket(AF_INET6, SOCK_STREAM, 0);
	sockaddr_in6 address6{};
	address6.sin6_family = AF_INET6;
	address6.sin6_addr = in6addr_loopback;
	length = sizeof address6;
	check(!::bind(listener6, reinterpret_cast<const sockaddr *>(&address6), length) && !::listen(listener6, 1) &&
		!::getsockname(listener6, reinterpret_cast<sockaddr *>(&address6), &length), "cannot listen on TCP over IPv6");
	std::snprintf(wanted, sizeof wanted, "00000000000000000000000001000000:%04X", unsigned(ntohs(address6.sin6_port)));
	check(raven_darwin::proc_stream("/proc/net/tcp6").str().find(wanted) != std::string::npos, std::string("/proc/net/tcp6 lists ") + wanted);
	::close(listener6);
}

void reopened_files()
{
	const auto path = std::filesystem::temp_directory_path() / ("raven-compat-lock-" + std::to_string(::getpid()));
	const int writer = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	check(writer >= 0 && ::write(writer, "lockable", 8) == 8, "cannot create a file to lock");
	const int reader = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
	const int reopened = raven_darwin_reopen(reader, O_RDWR | O_CLOEXEC);
	check(reopened >= 0 && (::fcntl(reopened, F_GETFL) & O_ACCMODE) == O_RDWR, "cannot reopen a file for writing");
	// The reopened file is a description of its own: its lock conflicts with one taken through another.
	struct flock lock{};
	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	lock.l_len = 4;
	check(!::fcntl(reopened, F_OFD_SETLK, &lock), "cannot lock the reopened file");
	struct flock other = lock;
	check(::fcntl(writer, F_OFD_SETLK, &other) < 0 && (errno == EAGAIN || errno == EACCES), "the reopened file's lock does not conflict");
	errno = 0;
	::close(reopened);
	::unlink(path.c_str());
	check(raven_darwin_reopen(reader, O_RDWR | O_CLOEXEC) < 0, "a removed file is reopened");
	errno = 0;
	::close(reader);
	::close(writer);
}

}

int main(int argc, char **argv)
{
	try {
		if (argc > 1) {
			const std::string_view option = argv[1];
			if (option.starts_with("--packet-child="))
				return packet_child(std::atoi(argv[1] + 15));
			if (option.starts_with("--bind-child="))
				return bind_child(std::string(option.substr(13)));
			if (option == "--sleep-child") {
				::sleep(30);
				return 0;
			}
		}
		packets();
		packets_across_spawn();
		abstract_names();
		process_files();
		tcp_table();
		reopened_files();
		std::cout << "darwin compat contract passed\n";
		return 0;
	} catch (const std::exception &failure) {
		std::cerr << failure.what() << '\n';
		return 1;
	}
}
