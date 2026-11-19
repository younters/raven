// SPDX-License-Identifier: MIT
#pragma once
#include "context.h"
// Original decoder for Microsoft's documented ARM64 PE unwind format.
// https://learn.microsoft.com/en-us/cpp/build/arm64-exception-handling
namespace RavenNT {
extern "C" void raven_capture_ec_context(void *);
extern "C" [[noreturn]] void raven_restore_ec_context(void *, void *);
[[noreturn]] void raise_status(int32_t status) {
	fprintf(stderr, "unhandled module NT exception %08x\n", uint32_t(status));
	std::abort();
}
struct RuntimeFunction {
	uint32_t begin, unwind;
};
uint32_t function_length(uint32_t unwind) {
	if (unwind & 3)
		return ((unwind >> 2) & 2047) * 4;
	return (active_image->bytes.get<uint32_t>(unwind) & 0x3ffff) * 4;
}
RuntimeFunction *lookup_function(uint64_t pc, uint64_t *base, void *) {
	if (!active_image)
		return nullptr;
	auto &image = *active_image;
	auto start = uintptr_t(image.bytes.data);
	if (pc < start || pc - start >= image.bytes.size)
		return nullptr;
	auto m = image.metadata();
	auto table = image.bytes.get<uint32_t>(m + 64), size = image.bytes.get<uint32_t>(m + 68);
	image.bytes.check(table, size);
	RuntimeFunction *found = nullptr;
	for (size_t at = table; at + 8 <= table + size; at += 8) {
		auto r = image.bytes.get<RuntimeFunction>(at);
		if (r.begin && pc - start >= r.begin && pc - start - r.begin < function_length(r.unwind)) {
			found = reinterpret_cast<RuntimeFunction *>(image.bytes.data + at);
			break;
		}
	}
	if (found && base)
		*base = start;
	return found;
}
struct UnwindState {
	uint8_t *context;
	uint64_t get(unsigned r) {
		if (r == 31) {
			uint64_t v;
			memcpy(&v, context + 0x98, 8);
			return v;
		}
		if (r >= 31 || !ec_offsets[r])
			return 0;
		uint64_t v;
		memcpy(&v, context + ec_offsets[r], 8);
		return v;
	}
	void set(unsigned r, uint64_t value) {
		if (r == 31)
			memcpy(context + 0x98, &value, 8);
		else if (r < 31 && ec_offsets[r])
			memcpy(context + ec_offsets[r], &value, 8);
	}
	void load(unsigned r, uint64_t address, unsigned kind = 0) {
		if (!kind) {
			uint64_t v;
			memcpy(&v, reinterpret_cast<void *>(address), 8);
			set(r, v);
		} else {
			if (r >= 16)
				raise_status(int32_t(0xc00000bb));
			memcpy(context + 0x1a0 + r * 16, reinterpret_cast<void *>(address), kind == 2 ? 16 : 8);
		}
	}
};
struct UnwindOp {
	uint8_t code;
	uint32_t operand;
	unsigned bytes;
};
std::vector<UnwindOp> decode_unwind(const uint8_t *p, size_t length, size_t index = 0) {
	std::vector<UnwindOp> result;
	for (size_t i = index; i < length;) {
		auto op = p[i++];
		unsigned more = op < 0xc0 ? 0
			: op < 0xdf			  ? 1
			: op == 0xe0		  ? 3
			: op == 0xe2		  ? 1
			: op == 0xe7		  ? 2
									: 0;
		if (more > length - i)
			raise_status(int32_t(0xc0000028));
		uint32_t operand = 0;
		for (unsigned j = 0; j < more; ++j)
			operand = (operand << 8) | p[i++];
		result.push_back({op, operand, 1 + more});
		if (op == 0xe4)
			break;
	}
	if (result.empty() || result.back().code != 0xe4)
		raise_status(int32_t(0xc0000028));
	return result;
}
void apply_ops(UnwindState &s, const std::vector<UnwindOp> &ops, size_t skip) {
	unsigned previous_reg = 0, previous_kind = 0;
	uint64_t previous_address = 0;
	auto save = [&](unsigned reg, unsigned n, uint64_t offset, uint64_t increment, unsigned kind) {
		auto address = s.get(31) + offset;
		for (unsigned j = 0; j < n; ++j)
			s.load(reg + j, address + j * (kind == 2 ? 16 : 8), kind);
		previous_reg = reg + n;
		previous_kind = kind;
		previous_address = address + n * (kind == 2 ? 16 : 8);
		s.set(31, s.get(31) + increment);
	};
	for (size_t i = skip; i < ops.size(); ++i) {
		auto o = ops[i];
		auto c = o.code;
		auto v = o.operand;
		if (c < 0x20)
			s.set(31, s.get(31) + (c & 31) * 16);
		else if (c < 0x40)
			save(19, 2, 0, (c & 31) * 8, 0);
		else if (c < 0x80)
			save(29, 2, (c & 63) * 8, 0, 0);
		else if (c < 0xc0)
			save(29, 2, 0, ((c & 63) + 1) * 8, 0);
		else if (c < 0xc8)
			s.set(31, s.get(31) + (((c & 7) << 8) | v) * 16);
		else if (c < 0xcc)
			save(19 + ((c & 3) << 2) + (v >> 6), 2, (v & 63) * 8, 0, 0);
		else if (c < 0xd0)
			save(19 + ((c & 3) << 2) + (v >> 6), 2, 0, ((v & 63) + 1) * 8, 0);
		else if (c < 0xd4)
			save(19 + ((c & 3) << 2) + (v >> 6), 1, (v & 63) * 8, 0, 0);
		else if (c < 0xd6)
			save(19 + ((c & 1) << 3) + (v >> 5), 1, 0, ((v & 31) + 1) * 8, 0);
		else if (c < 0xd8) {
			auto reg = 19 + 2 * (((c & 1) << 2) + (v >> 6));
			auto addr = s.get(31) + (v & 63) * 8;
			s.load(reg, addr);
			s.load(30, addr + 8);
		} else if (c < 0xda)
			save(8 + ((c & 1) << 2) + (v >> 6), 2, (v & 63) * 8, 0, 1);
		else if (c < 0xdc)
			save(8 + ((c & 1) << 2) + (v >> 6), 2, 0, ((v & 63) + 1) * 8, 1);
		else if (c < 0xde)
			save(8 + ((c & 1) << 2) + (v >> 6), 1, (v & 63) * 8, 0, 1);
		else if (c == 0xde)
			save(8 + (v >> 5), 1, 0, ((v & 31) + 1) * 8, 1);
		else if (c == 0xe0)
			s.set(31, s.get(31) + uint64_t(v) * 16);
		else if (c == 0xe1)
			s.set(31, s.get(29));
		else if (c == 0xe2)
			s.set(31, s.get(29) - v * 8);
		else if (c == 0xe3 || c == 0xe5)
			continue;
		else if (c == 0xe4)
			break;
		else if (c == 0xe6) {
			for (unsigned j = 0; j < 2; ++j)
				s.load(previous_reg + j, previous_address + j * (previous_kind == 2 ? 16 : 8),
					previous_kind);
			previous_reg += 2;
			previous_address += previous_kind == 2 ? 32 : 16;
		} else if (c == 0xe7) {
			auto b = v >> 8, reg = b & 31, kind = (v >> 6) & 3, n = (b & 64) ? 2u : 1u;
			bool pre = b & 32;
			if (kind == 3 || b & 128)
				raise_status(int32_t(0xc00000bb));
			auto unit = kind == 2 || pre || n == 2 ? 16 : 8;
			auto offset = (v & 63) * unit;
			save(reg, n, pre ? 0 : offset, pre ? offset + unit : 0, kind);
		} else
			raise_status(int32_t(0xc00000bb));
	}
}
void *virtual_unwind(uint32_t type, uint64_t base, uint64_t pc, RuntimeFunction *function,
	void *context, void **handler_data, uint64_t *frame, void *) {
	if (handler_data)
		*handler_data = nullptr;
	if (!context || !frame)
		raise_status(invalid_parameter);
	UnwindState state{static_cast<uint8_t *>(context)};
	*frame = state.get(31);
	if (!function) {
		auto lr = state.get(30);
		memcpy(state.context + 0xf8, &lr, 8);
		return nullptr;
	}
	if (!active_image || base != uintptr_t(active_image->bytes.data))
		raise_status(bad_handle);
	auto &b = active_image->bytes;
	uint32_t unwind = function->unwind;
	if (unwind & 3) {
		// Canonical packed body frame. Partial packed prologs/epilogs are rejected.
		unsigned ints = (unwind >> 16) & 15, fp = (unwind >> 13) & 7, chain = (unwind >> 21) & 3,
					home = (unwind >> 20) & 1;
		auto total = uint64_t(unwind >> 23) * 16;
		auto isz = ints * 8 + (chain == 1 ? 8 : 0), fsz = fp ? (fp + 1) * 8 : 0;
		auto saved = (isz + fsz + home * 64 + 15) & ~15u;
		if (total < saved)
			raise_status(int32_t(0xc0000028));
		auto local = total - saved, sp = state.get(31);
		if (pc - base - function->begin < 32)
			raise_status(int32_t(0xc00000bb));
		if (chain >= 2) {
			state.load(29, sp);
			state.load(30, sp + 8);
		}
		for (unsigned i = 0; i < ints; ++i)
			state.load(19 + i, sp + local + i * 8);
		if (chain == 1)
			state.load(30, sp + local + ints * 8);
		if (fp)
			for (unsigned i = 0; i <= fp; ++i)
				state.load(8 + i, sp + local + isz + i * 8, 1);
		state.set(31, sp + total);
	} else {
		auto header = b.get<uint32_t>(unwind);
		if ((header >> 18) & 3)
			raise_status(int32_t(0xc0000028));
		bool one = (header >> 21) & 1;
		uint32_t epilogs = (header >> 22) & 31, words = header >> 27;
		size_t start = unwind + 4;
		if (!words && !epilogs) {
			auto ext = b.get<uint32_t>(start);
			start += 4;
			epilogs = ext & 65535;
			words = (ext >> 16) & 255;
		}
		auto scopes = start;
		auto codes = start + (one ? 0 : epilogs * 4);
		b.check(codes, words * 4);
		auto *p = b.data + codes;
		auto ops = decode_unwind(p, words * 4);
		auto offset = (pc - base - function->begin) / 4;
		size_t skip = 0;
		bool in_epilog = false;
		if (one) {
			auto ep = decode_unwind(p, words * 4, epilogs);
			auto begin = (header & 0x3ffff) - ep.size();
			if (offset >= begin) {
				ops = std::move(ep);
				skip = offset - begin;
				in_epilog = true;
			}
		} else
			for (unsigned i = 0; i < epilogs; ++i) {
				auto scope = b.get<uint32_t>(scopes + i * 4);
				auto begin = scope & 0x3ffff;
				if (offset >= begin) {
					auto ep = decode_unwind(p, words * 4, scope >> 22);
					if (offset - begin < ep.size()) {
						ops = std::move(ep);
						skip = offset - begin;
						in_epilog = true;
						break;
					}
				}
			}
		if (!in_epilog && offset < ops.size() - 1)
			skip = ops.size() - 1 - offset;
		apply_ops(state, ops, skip);
		if (!in_epilog && !skip && type && (header & (1u << 20))) {
			auto handler = b.get<uint32_t>(codes + words * 4);
			b.check(handler, 4);
			if (handler_data)
				*handler_data = b.data + codes + words * 4 + 4;
			auto lr = state.get(30);
			memcpy(state.context + 0xf8, &lr, 8);
			return b.data + handler;
		}
	}
	auto lr = state.get(30);
	memcpy(state.context + 0xf8, &lr, 8);
	return nullptr;
}
[[noreturn]] void unwind_ex(
	void *target_frame, void *target_pc, void *record, void *value, void *context, void *history) {
	(void)record;
	// Context must be captured at the EC caller, not inside this Darwin adapter.
	// Refuse an invalid target rather than claiming a completed unwind.
	if (!context || !target_frame)
		raise_status(int32_t(0xc0000026));
	UnwindState state{static_cast<uint8_t *>(context)};
	for (unsigned depth = 0; depth < 1024 && state.get(31) < uintptr_t(target_frame); ++depth) {
		uint64_t pc;
		memcpy(&pc, state.context + 0xf8, 8);
		uint64_t base = 0, frame = 0;
		auto *fn = lookup_function(pc, &base, history);
		void *data = nullptr;
		if (virtual_unwind(2, base, pc, fn, context, &data, &frame, nullptr))
			raise_status(int32_t(0xc00000bb));
	}
	if (state.get(31) != uintptr_t(target_frame))
		raise_status(int32_t(0xc0000028));
	state.set(8, uintptr_t(value));
	auto pc = uintptr_t(target_pc);
	memcpy(state.context + 0xf8, &pc, 8);
	raven_restore_ec_context(context, nullptr);
}
void *resolve_unwind(const char *name) {
#define NT(n, f) \
	if (!strcmp(name, n)) \
	return reinterpret_cast<void *>(&f)
	NT("RtlLookupFunctionEntry", lookup_function);
	NT("RtlVirtualUnwind", virtual_unwind);
	NT("RtlUnwindEx", unwind_ex);
	NT("RtlRaiseStatus", raise_status);
#undef NT
	return nullptr;
}
} // namespace RavenNT
