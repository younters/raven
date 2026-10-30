#pragma once

#include "nt/kernel_context.h"

namespace raven_darwin {

// A null hToken selects the caller's primary token, including while impersonating.
// MSI uses this form to start its out-of-process custom-action server.
template<typename Create>
bool create_process_without_token(raven::ApiCall &call, raven::KernelContext &context, const Create &create)
{
	if (call[0]) return false;
	raven::ApiCall inner(call.process, call.environment,
		{call[1], call[2], call[3], call[4], call[5], call[6], call[7], call[8], call[9], call[10]});
	create(inner, context.process_token);
	call.ret(inner.result);
	return true;
}

}
