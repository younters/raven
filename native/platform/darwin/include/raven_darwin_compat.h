// Included into every host translation unit of the macOS build (see toolchain.cmake), ahead of any system header.
#pragma once

// IPV6_PKTINFO, IPV6_HOPLIMIT and the other RFC 3542 socket options, which Linux exposes by default.
#define __APPLE_USE_RFC_3542 1
