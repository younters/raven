#include "nt/providers.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

static std::vector<uint8_t> read_file(const std::filesystem::path &path)
{
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file || file.tellg() <= 0)
		throw std::runtime_error("cannot read folder browser fixture");
	std::vector<uint8_t> bytes(size_t(file.tellg()));
	file.seekg(0);
	if (!file.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(bytes.size())))
		throw std::runtime_error("short folder browser fixture");
	return bytes;
}

int main(int argc, char **argv)
{
	try {
		if (argc != 4)
			throw std::runtime_error("bootstrap, folder browser fixture and component directory required");
		raven::ModuleSet modules;
		auto &entry = modules.add("folder-browser.exe", read_file(argv[2]));
		const std::filesystem::path directory(argv[3]);
		for (const auto *name : {"comctl32.dll", "raven-user32.dll", "raven-shell.dll", "oleaut32.dll"})
			if (std::filesystem::exists(directory / name))
				modules.add(name, read_file(directory / name));
		if (entry.image.is_64 && std::filesystem::exists(directory / "raven-rtl.dll"))
			modules.add("raven-rtl.dll", read_file(directory / "raven-rtl.dll"));
		const auto source = [&](std::string_view name) -> std::optional<raven::ModuleBytes> {
			auto path = directory / name;
			if (name == "msvcrt.dll" && entry.image.is_64)
				path = directory / "ucrtbase.dll";
			if (name == "ucrtbase.dll" && !entry.image.is_64)
				path = directory.parent_path() / "ucrtbase32.dll";
			if (!std::filesystem::exists(path))
				return std::nullopt;
			return read_file(path);
		};
		// Visible controls require a presentation backend. Capture it in memory so the same dialog contract runs without a desktop server.
		raven::DesktopPresentation presentation;
		presentation.present = [](uint64_t, std::string_view, uint32_t, uint32_t, std::span<const uint8_t>) {};
		presentation.remove = [](uint64_t) {};
		presentation.poll = [](std::chrono::milliseconds) { return std::vector<raven::DesktopEvent>{}; };
		const auto result = entry.image.is_64 ?
			raven::run_native_process(modules, entry, argv[1], std::chrono::seconds(20), source, {}, presentation) :
			raven::run_native_process_x86(modules, entry, argv[1], std::chrono::seconds(20), {}, presentation, source);
		if (result.stop != raven::NativeStop::returned || result.value)
			throw std::runtime_error("folder browser SDK workflow failed: " + std::to_string(result.value));
		std::cout << "Folder selection, cancellation and task-allocated PIDLs passed\n";
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
	return 0;
}
