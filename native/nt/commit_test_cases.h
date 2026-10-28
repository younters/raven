// Fork-owned commit accounting cases, sharing the memory contract assertions.
#include <cstdio>
#include <unistd.h>

static void commit_test_cases(raven::NativeProcess &process, raven::NativeEnvironment &environment, raven::NativeMemory &memory)
{
	// Exhaustion must leave a cold reservation inaccessible, with no charge change.
	const auto cold=memory.allocate(0,32768,0x2000,4);
	check(!cold.status && memory.allocate(cold.address,20480,0x1000,4).status==0xc000012dU &&
		memory.committed()==0,"failed commit changed accounting");
	rejected([&] { process.read_memory(cold.address,1); },"readable");
	check(!memory.free(cold.address,0,0x8000).status,"cold reservation release failed");
	// Read-only file mappings consume address space but no private commitment.
	FILE *backing=std::tmpfile();
	check(backing && !ftruncate(fileno(backing),32768),"view backing failed");
	check(memory.map_view(fileno(backing),0,32768,4).status==0xc000012dU,"charged view exceeded limit");
	const auto readonly=memory.map_view(fileno(backing),0,32768,2,false);
	check(!readonly.status && memory.committed()==0,"read-only file view consumed commit");
	check(!memory.free(readonly.address,0,0x8000).status && memory.committed()==0,"read-only view release underflowed");
	std::fclose(backing);
	// Logged commits are charged once, including overlapping entries; decommit returns the charge.
	{
		raven::NativeMemory logged(process,environment,2*1024*1024);
		std::array<uint64_t,1024> block{};
		logged.set_commit_block(block.data());
		const auto r=logged.allocate(0,1024*1024,0x2000,4);
		check(!r.status && !logged.allocate(r.address,4096,0x1000,4).status,"log setup failed");
		check(block[raven::NativeMemory::commit_windows/8]==1,"fast commit window was not published");
		auto *count=reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(block.data())+raven::NativeMemory::commit_log);
		auto *entries=block.data()+(raven::NativeMemory::commit_log+8)/8;
		*count=2;
		entries[0]=entries[2]=r.address+4096;
		entries[1]=entries[3]=r.address+8192;
		check(logged.committed()==8192 && logged.committed()==8192,"log duplicated or lost a charge");
		const auto fill=logged.allocate(0,2*1024*1024-8192,0x3000,4);
		check(!fill.status && block[raven::NativeMemory::commit_windows/8]==0,"full budget still exposed a commit window");
		check(logged.allocate(r.address+8192,4096,0x1000,4).status==0xc000012dU,"log fallback exceeded limit");
		check(!logged.free(fill.address,0,0x8000).status && !logged.free(r.address,0,0x8000).status &&
			logged.committed()==0 && logged.peak_committed()==2*1024*1024,"log release or peak differs");
	}
}
