#include "memory.h"
#include "commit_guest.h"
#include <algorithm>

namespace raven {

// The sum of uncommitted bytes in every published window must fit the remaining budget. Overlapping windows are deliberately counted
// twice. Guest log entries can only consume this bounded set, even when a log fills before the next trap.
void NativeMemory::budget_windows()
{
	if (!commit_block) return;
	CommitBlockLock lock(commit_block);
	auto *windows=commit_block+commit_windows/8;
	windows[0]=std::min<uint64_t>(windows[0],commit_window_count);
	uint64_t potential=0;
	const auto headroom=commit_headroom();
	published_windows.clear();
	for (uint64_t i=0; i<windows[0];) {
		const auto base=windows[1+i*2], end=windows[2+i*2];
		const auto found=end>base && !((base|end)&4095) ? region(base,end-base) : regions.end();
		const auto fresh=found==regions.end() ? UINT64_MAX :
			end-base-found->second.pages.nonzero((base-found->first)/4096,(end-base)/4096)*4096;
		if (fresh>headroom-potential) {
			windows[1+i*2]=windows[1+(windows[0]-1)*2];
			windows[2+i*2]=windows[2+(windows[0]-1)*2];
			--windows[0];
		} else {
			potential+=fresh;
			published_windows.emplace_back(base,end);
			++i;
		}
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

NativeMemory::~NativeMemory()
{
	environment.set_memory_accounting(previous_accounting);
}

uint64_t NativeMemory::commit_headroom() const
{
	const uint64_t charge = system_commit_charge(used), system_limit = system_commit_limit();
	return std::min<uint64_t>(limit - std::min(limit,used), system_limit - std::min(system_limit,charge));
}

void NativeMemory::commit_changed()
{
	peak = std::max(peak,used);
	note_commit(used,peak);
	budget_windows();
}

void NativeMemory::commit_settle()
{
	if (!commit_block) return;
	CommitBlockLock lock(commit_block);
	auto *count = reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(commit_block)+commit_log);
	const auto logged = std::min<uint32_t>(*count,commit_log_count);
	const auto *entries = commit_block+(commit_log+8)/8;
	for (uint32_t i=0; i<logged; ++i) {
		const bool decommit = entries[i*2] & 1;
		const auto base = entries[i*2] & ~uint64_t(1), end = entries[i*2+1];
		if (end<=base || ((base|end)&4095) || !std::any_of(published_windows.begin(),published_windows.end(),
			[&](const auto &w) { return w.first<=base && end<=w.second; })) continue;
		const auto found = region(base,end-base);
		if (found==regions.end()) continue;
		const auto first = (base-found->first)/4096, pages = (end-base)/4096;
		const auto committed = found->second.pages.nonzero(first,pages);
		const auto fresh = found->second.charged ? (pages-committed)*4096 : 0;
		// A forged or obsolete entry cannot wrap the quota subtraction or exceed either budget.
		// Reject an entire over-capacity entry, leaving its uncharged pages reserved.
		if (!decommit && fresh>commit_headroom()) continue;
		found->second.pages.fill(first,pages,decommit ? 0 : 4);
		if (found->second.charged) {
			if (decommit) used-=std::min<uint64_t>(used,committed*4096);
			else used+=fresh;
		}
		used=std::min(used,limit);
		peak=std::max(peak,used);
		note_commit(used,peak);
	}
	*count=0;
	commit_block[commit_windows/8]=std::min<uint64_t>(commit_block[commit_windows/8],commit_window_count);
}

void NativeMemory::commit_open_window(uint64_t from, uint64_t to)
{
	if (!commit_block) return;
	CommitBlockLock lock(commit_block);
	auto *windows=commit_block+commit_windows/8;
	windows[0]=std::min<uint64_t>(windows[0],commit_window_count);
	for (uint64_t i=0; i<windows[0]; ++i)
		if (windows[1+i*2]<=to && from<=windows[2+i*2]) {
			windows[1+i*2]=std::min(windows[1+i*2],from);
			windows[2+i*2]=std::max(windows[2+i*2],to);
			return;
		}
	window_next=windows[0]<commit_window_count ? windows[0]++ : (window_next+1)%commit_window_count;
	windows[1+window_next*2]=from;
	windows[2+window_next*2]=to;
}

bool NativeMemory::commit_window_holds(uint64_t from, uint64_t to) const
{
	if (!commit_block) return false;
	CommitBlockLock lock(commit_block);
	const auto *windows=commit_block+commit_windows/8;
	for (uint64_t i=0; i<std::min<uint64_t>(windows[0],commit_window_count); ++i)
		if (windows[1+i*2]<=from && to<=windows[2+i*2]) return true;
	return false;
}

void NativeMemory::commit_close_windows(uint64_t from, uint64_t to)
{
	if (!commit_block) return;
	CommitBlockLock lock(commit_block);
	std::erase_if(published_windows,[&](const auto &w) { return w.first<to && from<w.second; });
	auto *windows=commit_block+commit_windows/8;
	windows[0]=std::min<uint64_t>(windows[0],commit_window_count);
	for (uint64_t i=0; i<windows[0];)
		if (windows[1+i*2]<to && from<windows[2+i*2]) {
			windows[1+i*2]=windows[1+(windows[0]-1)*2];
			windows[2+i*2]=windows[2+(windows[0]-1)*2];
			--windows[0];
		} else ++i;
}

}
