#pragma once
#include <string>

namespace raven_darwin {
// SearchPath searches directories only for an unqualified filename. MSI passes
// the full path of an extracted .tmp executable to launch its custom action.
inline void use_qualified_search_path(std::u16string &candidate, const std::u16string &file)
{
	if (file.find_first_of(u"\\/:") != std::u16string::npos) candidate = file;
}
}
