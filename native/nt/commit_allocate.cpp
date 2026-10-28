#include "memory.h"
#include <algorithm>
#include <sys/mman.h>

namespace raven {

static constexpr uint32_t invalid = 0xc000000dU, unsupported = 0xc00000bbU;
static constexpr uint32_t no_memory = 0xc0000017U, conflict = 0xc0000018U, commitment_limit = 0xc000012dU;

static int protection(uint32_t value)
{
	switch (value) {
	case 1: return PROT_NONE;
	case 2: return PROT_READ;
	case 8: case 4: return PROT_READ | PROT_WRITE;
	case 0x20: return PROT_READ | PROT_EXEC;
	case 0x80: case 0x40: return PROT_READ | PROT_WRITE | PROT_EXEC;
	default: return -1;
	}
}

// x86 guests keep Windows' default 2 GiB user range so every allocation stays positive as a signed pointer.
static constexpr uint64_t x86_user_limit = 0x7fff0000ULL;

static bool range(uint64_t address, uint64_t size, uint64_t user_limit)
{
	return size && address<user_limit && size<=user_limit-address;
}

NativeMemoryRange NativeMemory::commit_allocate(uint64_t address, uint64_t size, uint32_t type, uint32_t access)
{
	settle();
	if (!range(address,size,user_limit))
		return {0,0,invalid};
	// MEM_RESET preserves commitment and protection; eagerly discarding is optional on Windows.
	if (type == 0x80000) {
		const auto base=address & ~uint64_t(4095), length=(size+address-base+4095)&~uint64_t(4095);
		return region(base,length)==regions.end() ? NativeMemoryRange{0,0,conflict} : NativeMemoryRange{base,length};
	}
	// MEM_TOP_DOWN only asks for the highest free address; any free address serves the program as well.
	type&=~0x100000U;
	if (!(type & 0x3000) || (type & ~0x3000U))
		return {0,0,unsupported};
	const auto permissions=protection(access);
	if (permissions<0)
		return {0,0,0xc0000045U};
	const bool reserve=!address || (type & 0x2000);
	const uint64_t base=address & ~(reserve ? uint64_t(65535) : uint64_t(4095));
	const uint64_t length=(size+address-base+4095) & ~uint64_t(4095);
	if (!reserve) {
		const auto found=region(base,length);
		if (found==regions.end())
			return {0,0,conflict};
		const auto first=(base-found->first)/4096;
		const uint64_t fresh=found->second.charged ? (length/4096-found->second.pages.nonzero(first,length/4096))*4096 : 0;
		if (fresh>limit-used) return {0,0,commitment_limit};
		auto updated=found->second.pages;
		updated.fill(first,length/4096,access);
		// Programs such as a browser's allocators commit a few pages at a time, one block after the next, and each change of the guest's
		// protection stops it. Read-write commits of part of a 64 KiB granule therefore also open the neighbouring pages that are only
		// reserved, up to the 1 MiB around them; they stay reserved here, and committing them later changes nothing in the guest.
		// Commits of whole granules, as a WebAssembly memory grows with guard pages after it, open nothing more, and pages committed
		// otherwise are never touched.
		const uint64_t granule=((base | length) & 65535) ? uint64_t(1) << 20 : 4096;
		if (!process.has_protection(base,length,permissions)) {
			uint64_t from=base, to=base+length;
			if (permissions==(PROT_READ|PROT_WRITE)) {
				const uint64_t region_end=found->first+found->second.size;
				const uint64_t window_start=std::max(found->first,base & ~(granule-1));
				const uint64_t window_end=std::min(region_end,(base+length+granule-1) & ~(granule-1));
				while (from>window_start && !found->second.pages.at((from-4096-found->first)/4096))
					from-=4096;
				while (to<window_end && !found->second.pages.at((to-found->first)/4096))
					to+=4096;
			}
			close_windows(from,to);
			process.protect_memory(from,to-from,permissions);
			if (permissions==(PROT_READ|PROT_WRITE))
				open_window(from,to);
		} else if (permissions==(PROT_READ|PROT_WRITE)) {
			// Already open: its granule's open pages are published again, as the range that held them may have been dropped.
			const uint64_t region_end=found->first+found->second.size;
			const uint64_t window_start=std::max(found->first,base & ~(granule-1));
			const uint64_t window_end=std::min(region_end,(base+length+granule-1) & ~(granule-1));
			uint64_t from=base, to=base+length;
			while (from>window_start && process.has_protection(from-4096,4096,permissions))
				from-=4096;
			while (to<window_end && process.has_protection(to,4096,permissions))
				to+=4096;
			open_window(from,to);
		}
		// Only pages that were reserved until now count as new commitment.
		found->second.pages=std::move(updated);
		used+=fresh;
		peak=std::max(peak,used);
		budget_windows();
		return {base,length};
	}
	const bool commit_now=type & 0x1000;
	if (commit_now && length>limit-used) return {0,0,commitment_limit};
	if (regions.size()>=10000)
		return {0,0,no_memory};
	uint64_t selected=base;
	if (!address) {
		// The search starts after the last range handed out; at the top of the address space it goes on once from the bottom, over
		// the ranges freed since, as a 32-bit program that starts and ends many threads frees their stacks there.
		selected=next;
		bool wrapped=false;
		for (size_t tries=0; tries<20000; ++tries) {
			if (!range(selected,length,user_limit)) {
				if (wrapped)
					return {0,0,no_memory};
				wrapped=true;
				selected=floor;
				continue;
			}
			if (process.memory_available(selected,length))
				break;
			selected+=((length+65535)&~uint64_t(65535));
			if (tries==19999)
				return {0,0,no_memory};
		}
	}
	if (!selected || !process.memory_available(selected,length))
		return {0,0,conflict};
	const bool commit=commit_now;
	auto [found,inserted]=regions.emplace(selected,Region{length,access,PageMap(length/4096,commit ? access : 0),true});
	if (!inserted)
		return {0,0,conflict};
	try { process.map_memory(length,commit ? permissions : PROT_NONE,selected); }
	catch (...) { regions.erase(found); throw; }
	if (commit)
		used+=length;
	peak=std::max(peak,used);
	budget_windows();
	reserved_total+=length;
	if (!address)
		next=selected+((length+65535)&~uint64_t(65535));
	return {selected,length};
}

NativeMemoryRange NativeMemory::commit_map_view(int descriptor, uint64_t offset, uint64_t size, uint32_t access, bool charged)
{
	settle();
	const auto permissions=protection(access);
	if (permissions<0 || !size || (offset & 4095))
		return {0,0,invalid};
	const uint64_t length=(size+4095)&~uint64_t(4095);
	if (charged && length>limit-used) return {0,0,commitment_limit};
	if (regions.size()>=10000)
		return {0,0,no_memory};
	const auto address=process.map_shared(descriptor,length,offset);
	try {
		if (permissions!=(PROT_READ|PROT_WRITE)) process.protect_memory(address,length,permissions);
		regions.emplace(address,Region{length,access,PageMap(length/4096,access),charged});
	} catch (...) { process.unmap_memory(address); throw; }
	if (charged) used+=length;
	peak=std::max(peak,used);
	budget_windows();
	reserved_total+=length;
	return {address,length};
}

}
