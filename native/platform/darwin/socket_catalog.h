#pragma once
#include "nt/api.h"

namespace raven::darwin {

// Raven's catalogue contains only base TCP/UDP providers: there are no hidden or
// unchained LSP entries for WSC to add to WSAEnumProtocolsW's result. SPI failures
// report through lpErrno, preserving the calling thread's Winsock last error.
inline void register_socket_catalog(ApiRegistry &registry)
{
    const auto enumerate = registry.find("WSAEnumProtocolsW")->second;
    registry.add("WSCEnumProtocols", 4, [enumerate](ApiCall &call) {
        const auto saved_error = call.last_error();
        if (!call[3]) return call.ret(UINT64_MAX);
        uint32_t error = 0;
        try {
            enumerate(call);
            if (uint32_t(call.result) == UINT32_MAX) error = call.last_error();
        } catch (const ApiFault &) {
            error = 10014; // WSAEFAULT
            call.ret(UINT64_MAX);
        }
        call.set_last_error(saved_error);
        if (error) call.store<uint32_t>(call[3], error);
    });
}
}
