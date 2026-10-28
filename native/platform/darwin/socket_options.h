#pragma once
#include "nt/api.h"
#include <netinet/in.h>
#include <sys/socket.h>

namespace raven::darwin {
struct SocketOptions {
	bool exclusive = false;
	bool handle(ApiCall &call, int descriptor, bool setting)
	{
		if (call.u32(1) != 0xffff) return false;
		if (call.u32(2) == 4 && setting && exclusive) {
			call.fail(10022, UINT64_MAX); return true;
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
