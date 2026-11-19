// Voluntary port handoff only: no debugger attachment or entitlements.
#include "x87sidecar.h"
#include "coop_proto.h"

#include <libkern/OSCacheControl.h>
#include <servers/bootstrap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void raven_x87_handshake(void)
{
	const char *name = getenv(X87_COOP_ENV);
	char expected[64];
	snprintf(expected, sizeof expected, "x87sidecar.%d", getpid());
	if (!name || strcmp(name, expected)) return;
	mach_port_t service = MACH_PORT_NULL, reply = MACH_PORT_NULL, thread = MACH_PORT_NULL;
	kern_return_t result = bootstrap_look_up(bootstrap_port, name, &service);
	if (result != KERN_SUCCESS) goto done;
	result = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply);
	if (result != KERN_SUCCESS) goto done;
	thread = mach_thread_self();
	x87_coop_request_t request = {0};
	request.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE) | MACH_MSGH_BITS_COMPLEX;
	request.header.msgh_size = sizeof request;
	request.header.msgh_remote_port = service;
	request.header.msgh_local_port = reply;
	request.header.msgh_id = X87_COOP_MSGH_ID;
	request.body.msgh_descriptor_count = 2;
	request.task_port.name = mach_task_self();
	request.task_port.disposition = MACH_MSG_TYPE_COPY_SEND;
	request.task_port.type = MACH_MSG_PORT_DESCRIPTOR;
	request.thread_port.name = thread;
	request.thread_port.disposition = MACH_MSG_TYPE_COPY_SEND;
	request.thread_port.type = MACH_MSG_PORT_DESCRIPTOR;
	result = mach_msg(&request.header, MACH_SEND_MSG, sizeof request, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	if (result != KERN_SUCCESS) goto done;
	struct {
		x87_coop_reply_t message;
		mach_msg_trailer_t trailer;
	} received = {0};
	// Do not time out while the sidecar might be modifying live code. A dead
	// sidecar destroys its send-once right and wakes us with a notification.
	result = mach_msg(&received.message.header, MACH_RCV_MSG, 0, sizeof received, reply, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	if (result == KERN_SUCCESS) {
		if (received.message.header.msgh_id != X87_COOP_MSGH_ID + 1 || received.message.header.msgh_size != sizeof received.message)
			result = KERN_FAILURE;
		else {
			unsigned ranges = 0;
			for (unsigned i = 0; i < 2; ++i)
				if (received.message.icache_len[i]) {
					sys_icache_invalidate((void *)(uintptr_t)received.message.icache_addr[i], (size_t)received.message.icache_len[i]);
					++ranges;
				}
			if (getenv("RAVEN_TRACE_API"))
				fprintf(stderr, "raven: x87sidecar cooperative handshake: %u patched ranges invalidated\n", ranges);
		}
	}
done:
	if (reply != MACH_PORT_NULL) mach_port_mod_refs(mach_task_self(), reply, MACH_PORT_RIGHT_RECEIVE, -1);
	if (thread != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), thread);
	if (service != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), service);
	if (result != KERN_SUCCESS)
		fprintf(stderr, "raven: RUNNING WITHOUT X87 ACCELERATION: cooperative handshake failed (0x%x)\n", result);
	unsetenv(X87_COOP_ENV);
}
