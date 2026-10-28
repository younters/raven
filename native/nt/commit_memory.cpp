#include "memory.h"
#include <algorithm>

namespace raven {

// The sum of uncommitted bytes in every published window must fit the remaining budget. Overlapping windows are deliberately counted
// twice. Guest log entries can only consume this bounded set, even when a log fills before the next trap.
void NativeMemory::budget_windows()
{
	if (!commit_block) return;
	auto *windows=commit_block+commit_windows/8;
	uint64_t potential=0;
	for (uint64_t i=0; i<windows[0];) {
		const auto base=windows[1+i*2], end=windows[2+i*2];
		const auto found=region(base,end-base);
		const auto fresh=found==regions.end() ? end-base :
			end-base-found->second.pages.nonzero((base-found->first)/4096,(end-base)/4096)*4096;
		if (fresh>limit-used-potential) {
			windows[1+i*2]=windows[1+(windows[0]-1)*2];
			windows[2+i*2]=windows[2+(windows[0]-1)*2];
			--windows[0];
		} else { potential+=fresh; ++i; }
	}
}

std::pair<uint64_t,uint64_t> NativeMemory::address_space() const
{
	std::vector<std::pair<uint64_t,uint64_t>> spans;
	for (const auto &[base,r] : regions) spans.emplace_back(base,base+r.size);
	for (const auto &[base,r] : images) spans.emplace_back(base,base+r.size);
	spans.push_back(process.stack_bounds());
	std::sort(spans.begin(),spans.end());
	uint64_t occupied=0, end=0x10000;
	for (const auto &[a,b] : spans) {
		const auto first=std::max(a,end), last=std::min(b,user_limit);
		if (last>first) occupied+=last-first;
		end=std::max(end,last);
	}
	const auto total=user_limit-0x10000;
	return {total,total-std::min(total,occupied)};
}

}
