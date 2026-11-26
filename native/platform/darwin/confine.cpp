// macOS has no Landlock: the runtime is not confined by the kernel, and only the NT layer's sandbox checks keep a program to the files
// it is granted. With no confinement there is no broker either, so the brokered calls refuse; nothing calls them while confined() is
// false. Which images are the runtime's trusted programs is decided as on Linux.
#include "nt/confine.h"

#include <cerrno>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>
#include <system_error>

namespace raven {

bool trusted_image(const std::filesystem::path &image)
{
	std::error_code error;
	const auto runtime = std::filesystem::read_symlink("/proc/self/exe", error).parent_path();
	const auto file = std::filesystem::canonical(image, error);
	if (error)
		return false;
	const auto name = file.filename().string();
	return (file.parent_path() == runtime || file.parent_path() == runtime / "x86") && (name == "control.exe" || name == "raven-picker.exe");
}

bool confined()
{
	return false;
}

bool confine(const std::filesystem::path &, bool)
{
	return false;
}

// Unconfined, the system calls themselves.
int host_mkdir(const std::filesystem::path &host)
{
	return ::mkdir(host.c_str(), 0777);
}

int host_remove(const std::filesystem::path &host, bool directory)
{
	return directory ? ::rmdir(host.c_str()) : ::unlink(host.c_str());
}

int host_rename(const std::filesystem::path &from, const std::filesystem::path &to)
{
	return ::rename(from.c_str(), to.c_str());
}

int brokered_open(const std::filesystem::path &, int, mode_t)
{
	errno = ENOSYS;
	return -1;
}

std::pair<pid_t, int> brokered_spawn(const std::vector<std::string> &, const std::vector<std::pair<int, int>> &)
{
	errno = ENOSYS;
	return {-1, -1};
}

bool brokered_address(std::string_view)
{
	return false;
}

bool brokered_host(std::string_view, const std::vector<std::string> &)
{
	return false;
}

int host_broker_channel()
{
	return -1;
}

void adopt_host_broker(int) {}

bool host_brokered()
{
	return false;
}

bool forwarded_host(std::string_view, const std::vector<std::string> &, std::string_view)
{
	return false;
}

bool forwarded_address(std::string_view)
{
	return false;
}

}
