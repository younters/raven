// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <excpt.h>

static volatile LONG seen, continued, frame_seen;
static EXCEPTION_DISPOSITION __cdecl frame_handler(EXCEPTION_RECORD *record, void *frame, CONTEXT *context, void *dispatcher)
{
	(void)frame; (void)dispatcher;
	if (record->ExceptionCode != 0xe0424002 || !context->Eip || !context->Esp) return ExceptionContinueSearch;
	++frame_seen;
	return ExceptionContinueExecution;
}
static LONG CALLBACK exception(EXCEPTION_POINTERS *p)
{
	if (p->ExceptionRecord->ExceptionCode != 0xe0424001 || p->ExceptionRecord->NumberParameters != 2 ||
	    p->ExceptionRecord->ExceptionInformation[0] != 17 || p->ExceptionRecord->ExceptionInformation[1] != 23 ||
	    !p->ContextRecord->Eip || !p->ContextRecord->Esp) return EXCEPTION_CONTINUE_SEARCH;
	++seen;
	return EXCEPTION_CONTINUE_EXECUTION;
}
static LONG CALLBACK continuation(EXCEPTION_POINTERS *p)
{
	if (p->ExceptionRecord->ExceptionCode == 0xe0424001) ++continued;
	return EXCEPTION_CONTINUE_SEARCH;
}
int raven_entry(void)
{
	void *a = AddVectoredExceptionHandler(1, exception), *b = AddVectoredContinueHandler(1, continuation);
	const ULONG_PTR arguments[] = {17, 23};
	if (!a || !b) return 1;
	RaiseException(0xe0424001, 0, 2, arguments);
	if (seen != 1 || continued != 1) return 2;
	struct { void *previous; void *handler; } registration;
	registration.handler = frame_handler;
	__asm__ volatile("movl %%fs:0,%0" : "=r"(registration.previous));
	__asm__ volatile("movl %0,%%fs:0" :: "r"(&registration) : "memory");
	RaiseException(0xe0424002, 0, 0, NULL);
	__asm__ volatile("movl %0,%%fs:0" :: "r"(registration.previous) : "memory");
	if (frame_seen != 1) return 4;
	if (!RemoveVectoredExceptionHandler(a) || !RemoveVectoredContinueHandler(b)) return 3;
	return 0;
}
