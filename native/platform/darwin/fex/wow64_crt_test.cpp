// SPDX-License-Identifier: MIT
#include "fex_process.h"
#include "nt/providers.h"
#include <fstream>
#include <iostream>
#include <iterator>
int main(int argc, char **argv)
{
	try {
		if (argc != 4) return 2;
		raven::select_fex_execution(std::string_view(argv[3]) == "fex");
		std::ifstream file(argv[2], std::ios::binary);
		std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
		raven::ModuleSet modules;
		auto &entry = modules.add("cdecl.exe", std::move(bytes));
		// No guest CRT component: this must exercise ntdll's shared host trap seam.
		const auto result = raven::run_native_process_x86(modules, entry, argv[1], std::chrono::seconds(10));
		if (result.stop != raven::NativeStop::returned || result.value) throw std::runtime_error("ntdll memcpy corrupted the PE32 stack or result");
		std::cout << "PASS: ntdll memcpy preserves caller-cleanup stack\n";
		return 0;
	} catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
