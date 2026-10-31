#include "nt/prefix.h"
#include <filesystem>
#include <fstream>
#include <cassert>
#include <unistd.h>
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
    fs::permissions(file,fs::perms::none);
    bool copy_failed=false;
    try { prefix.place_runtime_components(source); } catch (const std::exception &) { copy_failed=true; }
    assert(copy_failed && fs::file_size(copy)==17);
    fs::permissions(file,fs::perms::owner_read|fs::perms::owner_write);
    fs::remove(copy); fs::create_directory(copy);
    bool failed=false;
    try { prefix.place_runtime_components(source); } catch (const std::exception &) { failed=true; }
    assert(failed);
    fs::remove_all(root);
}
