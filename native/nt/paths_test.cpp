#include "paths.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

// Two prefixes that hold the same Windows path each resolve it to their own files, in either order and again after the folders a lookup
// remembers are known: the remembered folders belong to one prefix's drive.
static void check(bool value, const char *message)
{
	if (!value)
		throw std::runtime_error(message);
}

int main()
{
	try {
		char pattern[] = "/tmp/raven-paths-XXXXXX";
		check(::mkdtemp(pattern) != nullptr, "cannot make a scratch folder");
		const std::filesystem::path scratch = pattern;
		for (const auto *name : {"first", "second"}) {
			std::filesystem::create_directories(scratch / name / "drive_c" / "Games" / "Data");
			std::ofstream(scratch / name / "drive_c" / "Games" / "Data" / "level.dat") << name;
		}
		const raven::NativePrefix first(scratch / "first"), second(scratch / "second");
		const raven::NativePaths first_paths(first), second_paths(second);
		for (int round = 0; round < 2; ++round)
			for (const auto *paths : {&first_paths, &second_paths}) {
				const auto resolved = paths->resolve(u"C:\\Games\\Data\\level.dat");
				const auto &root = paths == &first_paths ? first.root() : second.root();
				check(resolved && resolved->exists, "the file is not found");
				check(resolved->host == root / "drive_c" / "Games" / "Data" / "level.dat", "the file resolves into the other prefix");
			}
		std::filesystem::remove_all(scratch);
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
	return 0;
}
