#pragma once
#include <string>

namespace raven_darwin {
inline bool search_path_has_extension(const std::u16string &file)
{
	const auto slash = file.find_last_of(u"\\/:");
	return file.find(u'.', slash == file.npos ? 0 : slash + 1) != file.npos;
}

// SearchPath searches directories only for an unqualified filename. MSI passes
// the full path of an extracted .tmp executable to launch its custom action.
inline void use_qualified_search_path(std::u16string &candidate, const std::u16string &file)
{
	if (file.find_first_of(u"\\/:") != std::u16string::npos) candidate = file;
}
}
