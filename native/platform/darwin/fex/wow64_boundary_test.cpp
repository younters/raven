// SPDX-License-Identifier: MIT
#define RAVEN_FEX_EMBEDDED
#include "modhost/native_host.cpp"
#include "guest_window.h"
int main(int argc, char **argv)
{
	try {
		if (argc != 3) return 2;
		wow64_module = std::string_view(argv[2]) == "wow64";
		std::unique_ptr<raven::FexGuestWindow> window;
		if (wow64_module) { window = std::make_unique<raven::FexGuestWindow>(); wow64_guest_base = window->base(); }
		char *args[]{argv[0], argv[1]};
		return map_module(2, args, resolve, inspect_image, +[](Bytes bytes, size_t opt) {
			initialize_image(bytes, opt);
			verify_host_callret_fault();
		});
	} catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
