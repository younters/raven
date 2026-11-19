#pragma once
#include_next <netinet/tcp.h>

// The idle time before keepalive probes, which macOS calls TCP_KEEPALIVE.
#ifndef TCP_KEEPIDLE
#define TCP_KEEPIDLE TCP_KEEPALIVE
#endif
