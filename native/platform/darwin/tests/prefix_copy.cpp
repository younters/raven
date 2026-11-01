#include "nt/prefix.h"
#include <filesystem>
#include <fstream>
#include <cassert>
#include <unistd.h>
#include <sys/resource.h>
int main() {
    namespace fs=std::filesystem;
    char name[]="/tmp/raven-prefix-copy-XXXXXX";
    const fs::path root=mkdtemp(name), source=root/"runtime";
    fs::create_directories(source);
    const auto file=source/"msi.dll";
    std::ofstream(file)<<"first";
    fs::permissions(file,fs::perms::owner_read);
    raven::NativePrefix prefix(root/"prefix");
    prefix.place_runtime_components(source);
    const auto copy=root/"prefix/drive_c/windows/system32/msi.dll";
    assert(fs::file_size(copy)==5);
    assert((fs::status(copy).permissions() & fs::perms::owner_write)!=fs::perms::none);
    fs::permissions(file,fs::perms::owner_read);
    fs::permissions(file,fs::perms::owner_write,fs::perm_options::add);
    std::ofstream(file)<<"second and longer";
    fs::last_write_time(file,fs::last_write_time(copy)-std::chrono::hours(1));
    assert(fs::file_size(copy)==5);
    fs::permissions(file,fs::perms::owner_read);
    prefix.place_runtime_components(source);
    assert(fs::file_size(copy)==17);
    fs::permissions(file,fs::perms::owner_write,fs::perm_options::add);
    std::ofstream(file)<<"third";
    // Exhaust descriptors to fail mkstemp independently of root permissions.
    struct rlimit saved, limited;
    assert(!getrlimit(RLIMIT_NOFILE,&saved)); limited=saved; limited.rlim_cur=0;
    assert(!setrlimit(RLIMIT_NOFILE,&limited));
    prefix.place_runtime_components(source);
    assert(!setrlimit(RLIMIT_NOFILE,&saved));
    assert(fs::file_size(copy)==17);
    // Rename over a nonempty directory fails even for root, after copying.
    fs::remove(copy); fs::create_directory(copy);
    std::ofstream(copy/"keep")<<"existing";
    std::ofstream(copy.string()+".tmp.abandoned")<<"stale";
    fs::last_write_time(copy.string()+".tmp.abandoned", fs::file_time_type::clock::now()-std::chrono::hours(48));
    prefix.place_runtime_components(source);
    assert(fs::is_regular_file(copy/"keep"));
    assert(!fs::exists(copy.string()+".tmp.abandoned"));
    fs::remove_all(copy);
    prefix.place_runtime_components(source);
    assert(fs::file_size(copy)==5);
    fs::remove_all(root);
}
