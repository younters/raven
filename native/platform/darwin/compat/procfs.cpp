// The /proc files the runtime reads as a process API, in the format Linux writes them, made from libproc, sysctl and Mach.

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <cstring>
#include <sys/param.h>
#include <sys/proc_info.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace {

// Linux reports memory in its 4 KiB pages, and so does the runtime (see raven_darwin_sysconf).
constexpr uint64_t page = 4096;

uint64_t nanoseconds(uint64_t mach_time)
{
	static const mach_timebase_info_data_t timebase = [] {
		mach_timebase_info_data_t value{};
		mach_timebase_info(&value);
		if (!value.denom)
			value = {1, 1};
		return value;
	}();
	return mach_time * timebase.numer / timebase.denom;
}

std::vector<pid_t> process_ids()
{
	const int expected = proc_listallpids(nullptr, 0);
	if (expected <= 0)
		return {};
	std::vector<pid_t> pids(size_t(expected) + 64);
	const int count = proc_listallpids(pids.data(), int(pids.size() * sizeof(pid_t)));
	pids.resize(size_t(std::max(count, 0)));
	// Linux lists no process 0; macOS's is the kernel.
	std::erase(pids, 0);
	std::sort(pids.begin(), pids.end());
	return pids;
}

std::string stat_line(pid_t pid)
{
	proc_bsdinfo info{};
	if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof info) != int(sizeof info)) {
		// libproc has nothing on a process that ended and was not reaped yet; the process table still lists it.
		kinfo_proc entry{};
		size_t size = sizeof entry;
		int query[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, int(pid)};
		if (sysctl(query, 4, &entry, &size, nullptr, 0) != 0 || size != sizeof entry || entry.kp_proc.p_pid != pid)
			return {};
		info.pbi_status = uint32_t(entry.kp_proc.p_stat);
		info.pbi_ppid = uint32_t(entry.kp_eproc.e_ppid);
		info.pbi_pgid = uint32_t(entry.kp_eproc.e_pgid);
		info.pbi_start_tvsec = uint64_t(entry.kp_proc.p_starttime.tv_sec);
		info.pbi_start_tvusec = uint64_t(entry.kp_proc.p_starttime.tv_usec);
		std::memcpy(info.pbi_comm, entry.kp_proc.p_comm, std::min(sizeof info.pbi_comm, sizeof entry.kp_proc.p_comm));
	}
	proc_taskinfo task{};
	if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &task, sizeof task) != int(sizeof task))
		task = {};
	static constexpr char states[] = {'R', 'R', 'R', 'S', 'T', 'Z'};
	const char state = info.pbi_status < sizeof states ? states[info.pbi_status] : 'R';
	const uint64_t ticks = uint64_t(sysconf(_SC_CLK_TCK)), tick = 1000000000ULL / (ticks ? ticks : 100);
	timeval boot{};
	size_t size = sizeof boot;
	sysctlbyname("kern.boottime", &boot, &size, nullptr, 0);
	const int64_t started = (int64_t(info.pbi_start_tvsec) - boot.tv_sec) * int64_t(ticks) +
		(int64_t(info.pbi_start_tvusec) - boot.tv_usec) * int64_t(ticks) / 1000000;
	const char *name = info.pbi_name[0] ? info.pbi_name : info.pbi_comm;
	const pid_t session = getsid(pid);
	char line[512];
	snprintf(line, sizeof line, "%d (%s) %c %d %d %d 0 -1 0 %llu 0 %llu 0 %llu %llu 0 0 %d %d %d 0 %lld %llu %llu\n", int(pid), name, state,
		int(info.pbi_ppid), int(info.pbi_pgid), int(session < 0 ? 0 : session), static_cast<unsigned long long>(task.pti_faults),
		static_cast<unsigned long long>(task.pti_pageins), static_cast<unsigned long long>(nanoseconds(task.pti_total_user) / tick),
		static_cast<unsigned long long>(nanoseconds(task.pti_total_system) / tick), int(task.pti_priority), int(info.pbi_nice),
		int(task.pti_threadnum), static_cast<long long>(std::max<int64_t>(started, 0)),
		static_cast<unsigned long long>(task.pti_virtual_size), static_cast<unsigned long long>(task.pti_resident_size / page));
	return line;
}

std::string command_line(pid_t pid)
{
	int maximum = 0;
	size_t size = sizeof maximum;
	int query[2] = {CTL_KERN, KERN_ARGMAX};
	if (sysctl(query, 2, &maximum, &size, nullptr, 0) != 0 || maximum <= 0)
		maximum = 1 << 20;
	std::vector<char> buffer(static_cast<size_t>(maximum));
	int arguments[3] = {CTL_KERN, KERN_PROCARGS2, int(pid)};
	size = buffer.size();
	if (sysctl(arguments, 3, buffer.data(), &size, nullptr, 0) != 0 || size < sizeof(int))
		return {};
	// The argument count, the executable's path, padding, then the arguments and the environment, each ending in a zero.
	int count;
	memcpy(&count, buffer.data(), sizeof count);
	const char *at = buffer.data() + sizeof count, *end = buffer.data() + size;
	at = std::find(at, end, '\0');
	at = std::find_if(at, end, [](char character) { return character != '\0'; });
	std::string text;
	for (int index = 0; index < count && at < end; ++index) {
		const char *stop = std::find(at, end, '\0');
		text.append(at, stop);
		text += '\0';
		at = stop + 1;
	}
	return text;
}

std::string statm()
{
	task_vm_info_data_t info{};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
		return {};
	// The size is the memory the process has committed: the virtual size of a macOS process counts the whole shared cache.
	char line[128];
	snprintf(line, sizeof line, "%llu %llu 0 0 0 %llu 0\n", static_cast<unsigned long long>(info.phys_footprint / page),
		static_cast<unsigned long long>(info.resident_size / page), static_cast<unsigned long long>(info.phys_footprint / page));
	return line;
}

struct TcpSocket {
	uint64_t id;
	pid_t pid;
	socket_info info;
};

// The TCP sockets of every process this user may inspect, each once, with the first process that holds it.
std::vector<TcpSocket> tcp_sockets()
{
	std::vector<TcpSocket> sockets;
	std::set<uint64_t> seen;
	for (const pid_t pid : process_ids()) {
		const int bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
		if (bytes <= 0)
			continue;
		std::vector<proc_fdinfo> descriptors(size_t(bytes) / sizeof(proc_fdinfo) + 16);
		const int listed = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, descriptors.data(), int(descriptors.size() * sizeof(proc_fdinfo)));
		if (listed <= 0)
			continue;
		descriptors.resize(size_t(listed) / sizeof(proc_fdinfo));
		for (const auto &descriptor : descriptors) {
			if (descriptor.proc_fdtype != PROX_FDTYPE_SOCKET)
				continue;
			socket_fdinfo info{};
			if (proc_pidfdinfo(pid, descriptor.proc_fd, PROC_PIDFDSOCKETINFO, &info, sizeof info) != int(sizeof info))
				continue;
			if (info.psi.soi_kind != SOCKINFO_TCP || !seen.insert(info.psi.soi_so).second)
				continue;
			sockets.push_back({info.psi.soi_so, pid, info.psi});
		}
	}
	return sockets;
}

std::string address_words(const in_sockinfo &endpoint, bool local, bool v6)
{
	const void *v4 = local ? static_cast<const void *>(&endpoint.insi_laddr.ina_46.i46a_addr4) : &endpoint.insi_faddr.ina_46.i46a_addr4;
	const void *v6_address = local ? static_cast<const void *>(&endpoint.insi_laddr.ina_6) : &endpoint.insi_faddr.ina_6;
	uint32_t words[4] = {};
	size_t count = 1;
	if (!v6)
		memcpy(words, v4, 4);
	else {
		count = 4;
		if (endpoint.insi_vflag & INI_IPV6)
			memcpy(words, v6_address, 16);
		else {
			// An IPv6 socket talking IPv4 shows the IPv4-mapped address, as Linux does.
			words[2] = htonl(0xffff);
			memcpy(&words[3], v4, 4);
		}
	}
	std::string text;
	for (size_t index = 0; index < count; ++index) {
		char word[9];
		snprintf(word, sizeof word, "%08X", words[index]);
		text += word;
	}
	const int port = local ? endpoint.insi_lport : endpoint.insi_fport;
	char suffix[8];
	snprintf(suffix, sizeof suffix, ":%04X", unsigned(ntohs(uint16_t(port))));
	return text + suffix;
}

std::string tcp_table(bool v6)
{
	// BSD's TCPS_* states as Linux numbers them.
	static constexpr unsigned states[] = {7, 10, 2, 3, 1, 8, 4, 11, 9, 5, 6};
	std::string text = "  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt"
		"   uid  timeout inode\n";
	size_t slot = 0;
	for (const auto &socket : tcp_sockets()) {
		if (socket.info.soi_family != (v6 ? AF_INET6 : AF_INET))
			continue;
		const auto &tcp = socket.info.soi_proto.pri_tcp;
		const unsigned state = tcp.tcpsi_state >= 0 && size_t(tcp.tcpsi_state) < std::size(states) ? states[tcp.tcpsi_state] : 7;
		char line[256];
		snprintf(line, sizeof line, "%4zu: %s %s %02X 00000000:00000000 00:00000000 00000000 %5u        0 %llu 1 0000000000000000 20 4 30 10 -1\n",
			slot++, address_words(tcp.tcpsi_ini, true, v6).c_str(), address_words(tcp.tcpsi_ini, false, v6).c_str(), state,
			unsigned(socket.info.soi_stat.vst_uid), static_cast<unsigned long long>(socket.id));
		text += line;
	}
	return text;
}

}

pid_t raven_darwin::proc_process(std::string_view path, std::string_view leaf)
{
	constexpr std::string_view root = "/proc/";
	if (!path.starts_with(root))
		return -1;
	path.remove_prefix(root.size());
	const auto slash = path.find('/');
	if (slash == path.npos || path.substr(slash + 1) != leaf)
		return -1;
	const auto name = path.substr(0, slash);
	if (name == "self")
		return getpid();
	if (name.empty() || name.size() > 9 || !std::all_of(name.begin(), name.end(), [](char character) { return character >= '0' && character <= '9'; }))
		return -1;
	return pid_t(std::stol(std::string(name)));
}

std::istringstream raven_darwin::proc_stream(const std::filesystem::path &path)
{
	const auto name = path.string();
	if (name == "/proc/net/tcp" || name == "/proc/net/tcp6")
		return std::istringstream(tcp_table(name.back() == '6'));
	if (const pid_t pid = proc_process(name, "stat"); pid >= 0)
		return std::istringstream(stat_line(pid));
	if (const pid_t pid = proc_process(name, "cmdline"); pid >= 0)
		return std::istringstream(command_line(pid));
	if (proc_process(name, "statm") == getpid())
		return std::istringstream(statm());
	std::ifstream file(path, std::ios::binary);
	return std::istringstream(std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()));
}

std::vector<raven_darwin::ProcEntry> raven_darwin::proc_directory(std::error_code &error)
{
	error.clear();
	std::vector<ProcEntry> entries;
	for (const pid_t pid : process_ids())
		entries.push_back({std::filesystem::path("/proc") / std::to_string(pid)});
	if (entries.empty())
		error = std::make_error_code(std::errc::permission_denied);
	return entries;
}

std::map<uint64_t, uint32_t> raven_darwin::socket_owners()
{
	std::map<uint64_t, uint32_t> owners;
	for (const auto &socket : tcp_sockets())
		owners.emplace(socket.id, uint32_t(socket.pid));
	return owners;
}

extern "C" int raven_darwin_reopen(int descriptor, int flags)
{
	char path[MAXPATHLEN];
	if (fcntl(descriptor, F_GETPATH, path) == -1)
		return -1;
	const int reopened = open(path, flags);
	if (reopened < 0)
		return -1;
	struct stat original, opened;
	if (fstat(descriptor, &original) != 0 || fstat(reopened, &opened) != 0 || original.st_dev != opened.st_dev || original.st_ino != opened.st_ino) {
		close(reopened);
		errno = ENOENT;
		return -1;
	}
	return reopened;
}
