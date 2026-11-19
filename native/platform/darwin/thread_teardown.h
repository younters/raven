#pragma once

namespace raven_darwin {

// A timeout can leave the main coroutine marked waiting. Its wait predicate no
// longer matters during destruction: unwound workers must hand control back to
// this host frame, not wait forever for the guest event that timed out.
template<typename Thread>
void prepare_teardown_caller(Thread &thread)
{
	thread.state = Thread::State::running;
}

}
