#pragma once
#include "nt/api.h"
#include <algorithm>
#include <netinet/in.h>
#include <sys/socket.h>

namespace raven::darwin {
struct SocketOptions {
	bool exclusive = false;
	bool handle(ApiCall &call, int descriptor, bool setting)
	{
		if (call.u32(1) != 0xffff) return false;
		if (call.u32(2) == 4 && setting && exclusive) {
			if (!call[3] || call.i32(4) < 1) { call.fail(10014, UINT64_MAX); return true; }
			// Winsock reads a Boolean option from as little as one byte, as the generic setsockopt path does.
			const auto value = call.read(call[3], std::min<size_t>(size_t(call.i32(4)), sizeof(int32_t)));
			int32_t reuse = 0;
			for (size_t at = 0; at < value.size(); ++at)
				reuse |= int32_t(value[at]) << (8 * at);
			if (reuse) { call.fail(10022, UINT64_MAX); return true; }
			// Disabling reuse preserves exclusivity and is a valid Boolean option.
			call.ret(0); return true;
		}
		if (call.u32(2) != 0xfffffffbU) return false; // SO_EXCLUSIVEADDRUSE
		if (!call[3] || !call[4] || (setting ? call.u32(4) : call.load<uint32_t>(call[4])) < 4) {
			call.fail(10014, UINT64_MAX); return true;
		}
		if (setting) {
			sockaddr_storage address{};
			socklen_t length = sizeof(address);
			if (::getsockname(descriptor, reinterpret_cast<sockaddr *>(&address), &length) ||
				(address.ss_family == AF_INET && reinterpret_cast<sockaddr_in *>(&address)->sin_port) ||
				(address.ss_family == AF_INET6 && reinterpret_cast<sockaddr_in6 *>(&address)->sin6_port)) {
				call.fail(10022, UINT64_MAX); return true;
			}
			const bool requested = call.load<int32_t>(call[3]) != 0;
			int reuse = 0;
			length = sizeof(reuse);
			::getsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &reuse, &length);
			if (requested && reuse) { call.fail(10022, UINT64_MAX); return true; }
			// BSD sockets already reject competing binds unless reuse is enabled. Keep
			// it disabled and reject later reuse requests for an exclusive socket.
			exclusive = requested;
		} else {
			call.store<int32_t>(call[3], exclusive);
			call.store<uint32_t>(call[4], 4);
		}
		call.ret(0);
		return true;
	}
};
}
