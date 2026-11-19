#pragma once
#include "nt/kernel_context.h"
#include "nt/text.h"
#include <atomic>

namespace raven::darwin {
// ANSI SSPI uses the same credential/context handles and TLS engine as Unicode
// SSPI. Only the package, principal and target strings differ for these calls.
inline void register_schannel_ansi(ApiRegistry &registry, KernelContext &context)
{
	struct Adapter { const char *narrow, *wide; std::vector<size_t> strings; size_t words; };
	for (const auto &entry : {
		Adapter{"AcquireCredentialsHandleA", "AcquireCredentialsHandleW", {0,1}, 9},
		Adapter{"InitializeSecurityContextA", "InitializeSecurityContextW", {2}, 12},
		// The upstream provider supports only binary attributes (sizes, connection,
		// ALPN); all have identical ANSI/Unicode layouts.
		Adapter{"QueryContextAttributesA", "QueryContextAttributesW", {}, 3},
		Adapter{"QueryCredentialsAttributesA", "QueryCredentialsAttributesW", {}, 3},
		Adapter{"ImportSecurityContextA", "ImportSecurityContextW", {0}, 4},
		Adapter{"AddCredentialsA", "AddCredentialsW", {1,2}, 8},
		Adapter{"SetContextAttributesA", "SetContextAttributesW", {}, 4},
		Adapter{"SetCredentialsAttributesA", "SetCredentialsAttributesW", {}, 4},
		Adapter{"ChangeAccountPasswordA", "ChangeAccountPasswordW", {0,1,2,3,4}, 8}}) {
		// An ANSI form the shared provider already has is its own; the adapter only fills in the ones it lacks.
		if (registry.find(entry.narrow))
			continue;
		const auto base = registry.find(entry.wide);
		if (!base) {
			registry.add(entry.narrow, entry.words, [](ApiCall &call) { call.ret(0x80090302U); });
			continue;
		}
		registry.add(entry.narrow, base->first, [entry, base, &context](ApiCall &call) {
			struct Scratch {
				GuestAllocator &allocator;
				std::vector<uint64_t> blocks;
				~Scratch() { for (auto block : blocks) allocator.release(block); }
			} scratch{context.allocator, {}};
			std::vector<uint64_t> args;
			for (size_t i=0; i<base->first; ++i) args.push_back(call[i]);
			for (auto i : entry.strings) if (args[i]) {
				const auto value = from_ansi(call.narrow(args[i]), 1252);
				const auto block = context.allocator.allocate((value.size()+1)*2);
				if (!block) { call.ret(0x80090300U); return; }
				scratch.blocks.push_back(block);
				call.put_wide(block, value, value.size()+1);
				args[i] = block;
			}
			ApiCall inner(call.process, call.environment, args);
			base->second(inner);
			call.ret(inner.result);
		});
	}
	if (registry.find("InitSecurityInterfaceA"))
		return;
	const auto wide_entry = registry.find("InitSecurityInterfaceW");
	if (!wide_entry) {
		registry.add("InitSecurityInterfaceA", 0, [](ApiCall &call) { call.fail(120); });
		return;
	}
	const auto wide = wide_entry->second;
	auto published = std::make_shared<std::atomic<uint64_t>>(0);
	registry.add("InitSecurityInterfaceA", 0, [wide, published, &context](ApiCall &call) {
		if (auto table = published->load()) { call.ret(table); return; }
		wide(call);
		if (!call.result) return;
		const auto width = call.pointer_width();
		const auto bytes = call.read(call.result, 32*width);
		const auto table = context.allocator.allocate(bytes.size());
		if (!table) { call.fail(8); return; }
		call.write(table, bytes);
		struct Slot { size_t index; const char *name; };
		for (auto slot : {Slot{0,"EnumerateSecurityPackagesA"}, Slot{1,"QueryCredentialsAttributesA"},
			Slot{2,"AcquireCredentialsHandleA"}, Slot{5,"InitializeSecurityContextA"},
			Slot{10,"QueryContextAttributesA"}, Slot{16,"QuerySecurityPackageInfoA"},
			Slot{20,"ImportSecurityContextA"}, Slot{21,"AddCredentialsA"},
			Slot{26,"SetContextAttributesA"}, Slot{27,"SetCredentialsAttributesA"}, Slot{28,"ChangeAccountPasswordA"}})
			call.store_pointer(table+(slot.index+1)*width, context.modules.resolve({"secur32.dll",slot.name,0,false,0}));
		uint64_t expected = 0;
		if (!published->compare_exchange_strong(expected, table)) context.allocator.release(table);
		call.ret(published->load());
	});
}
}
