#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <vector>

namespace raven_darwin {

inline std::vector<std::filesystem::path> font_roots()
{
	std::vector<std::filesystem::path> roots{"/usr/share/fonts", "/usr/local/share/fonts", "/System/Library/Fonts", "/Library/Fonts"};
	if (const char *home = std::getenv("HOME"); home && *home) {
		roots.emplace_back(std::filesystem::path(home) / "Library/Fonts");
		roots.emplace_back(std::filesystem::path(home) / ".local/share/fonts");
	}
	return roots;
}

inline std::vector<std::filesystem::path> font_files()
{
	std::vector<std::filesystem::path> files;
	for (const auto &root : font_roots()) {
		std::error_code error;
		for (std::filesystem::recursive_directory_iterator walk(root, std::filesystem::directory_options::skip_permission_denied |
				std::filesystem::directory_options::follow_directory_symlink, error), end; walk != end; walk.increment(error)) {
			if (error || !walk->is_regular_file(error))
				continue;
			auto extension = walk->path().extension().string();
			for (auto &character : extension)
				character = char(std::tolower(static_cast<unsigned char>(character)));
			if (extension == ".ttf" || extension == ".otf" || extension == ".ttc" || extension == ".otc")
				files.push_back(walk->path());
		}
	}
	std::sort(files.begin(), files.end());
	files.erase(std::unique(files.begin(), files.end()), files.end());
	return files;
}

}
