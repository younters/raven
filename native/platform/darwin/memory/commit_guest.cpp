#include "commit_guest.h"
#include <cstring>

namespace raven {
#include "commit_guest.inc"

size_t commit_guest_jump(bool decommit) { return decommit ? 81 : 91; }
std::vector<uint8_t> commit_guest_prelude(int32_t pointer, int32_t trap, bool decommit)
{
	auto code = decommit ? decommit_prelude_locked : commit_prelude_locked;
	const int32_t displacement = pointer - 7;
	std::memcpy(code.data() + 3, &displacement, 4);
	const auto jump = commit_guest_jump(decommit);
	const int32_t relative = trap - int32_t(jump + 5);
	std::memcpy(code.data() + jump + 1, &relative, 4);
	return code;
}
const std::vector<uint8_t> &commit_guest_nt_prelude(bool decommit)
{
	return decommit ? nt_decommit_prelude_locked : nt_commit_prelude_locked;
}
}
