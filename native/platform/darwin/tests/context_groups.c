#include <windows.h>
static volatile LONG ready, release_worker, progress;
static unsigned long long observed_xmm[2];
static DWORD observed_mxcsr;
static unsigned char observed_x87[10];
static const unsigned long long vector[2] __attribute__((aligned(16))) = {0x1122334455667788ULL,0x8877665544332211ULL};
static const DWORD mxcsr = 0x3f80;
__attribute__((target("sse2"))) static DWORD WINAPI worker(void *arg) {
    __asm__ volatile("fld1; movdqu %0, %%xmm7; ldmxcsr %1" :: "m"(vector), "m"(mxcsr) : "xmm7");
    ready = 1;
    while (!release_worker) { ++progress; __asm__ volatile("pause"); }
    __asm__ volatile("movdqu %%xmm7, %0; stmxcsr %1; fstpt %2" : "=m"(observed_xmm), "=m"(observed_mxcsr), "=m"(observed_x87));
    return 0;
}
#define CHECK(x) do { if (!(x)) { release_worker=1; return __LINE__; } } while (0)
int raven_entry(void) {
    CONTEXT self = {0}, ctx = {0}, back = {0};
    self.ContextFlags = CONTEXT_ALL;
    CHECK(GetThreadContext(GetCurrentThread(), &self));
    CHECK(self.SegCs && self.SegSs);
    typedef BOOL (WINAPI *initialize_fn)(PVOID,DWORD,PCONTEXT *,PDWORD);
    typedef BOOL (WINAPI *copy_fn)(PCONTEXT,DWORD,PCONTEXT);
    HMODULE kernel=GetModuleHandleA("kernel32.dll");
    initialize_fn initialize=(initialize_fn)GetProcAddress(kernel,"InitializeContext");
    copy_fn copy=(copy_fn)GetProcAddress(kernel,"CopyContext");
    CHECK(initialize && copy);
    static unsigned char storage[2048]; PCONTEXT extended=0; DWORD length=0;
    CHECK(!initialize(0,CONTEXT_ALL|0x40,&extended,&length) && GetLastError()==ERROR_INSUFFICIENT_BUFFER);
    CHECK(length<=sizeof storage);
    CHECK(initialize(storage,CONTEXT_ALL|0x40,&extended,&length));
    CHECK(GetThreadContext(GetCurrentThread(),extended));
    CHECK(copy(&self,CONTEXT_ALL,extended));
    HANDLE thread = CreateThread(0,0,worker,0,0,0);
    CHECK(thread);
    while (!ready) Sleep(1);
    ctx.ContextFlags = CONTEXT_ALL;
    CHECK(GetThreadContext(thread,&ctx));
    LONG before=progress;
    for (unsigned i=0;i<10000000 && progress==before;++i) __asm__ volatile("pause");
    CHECK(progress!=before);
    CHECK(SuspendThread(thread)==0);
    CHECK(GetThreadContext(thread,&ctx));
    CHECK(ctx.SegCs && ctx.SegSs);
    typedef LONG (WINAPI *nt_context_fn)(HANDLE, void *);
    HMODULE ntdll=GetModuleHandleA("ntdll.dll");
    nt_context_fn ntget=(nt_context_fn)GetProcAddress(ntdll,"NtGetContextThread");
    nt_context_fn ntset=(nt_context_fn)GetProcAddress(ntdll,"NtSetContextThread");
    CHECK(ntget && ntset && ntget(thread,&ctx)==0);
#ifdef _WIN64
    typedef PRUNTIME_FUNCTION (WINAPI *lookup_fn)(DWORD64,PDWORD64,PUNWIND_HISTORY_TABLE);
    typedef PEXCEPTION_ROUTINE (WINAPI *unwind_fn)(DWORD,DWORD64,DWORD64,PRUNTIME_FUNCTION,PCONTEXT,PVOID *,PDWORD64,PKNONVOLATILE_CONTEXT_POINTERS);
    lookup_fn lookup=(lookup_fn)GetProcAddress(ntdll,"RtlLookupFunctionEntry");
    unwind_fn unwind=(unwind_fn)GetProcAddress(ntdll,"RtlVirtualUnwind");
    CHECK(lookup && unwind);
    DWORD64 base=0, frame=0; void *data=0;
    PRUNTIME_FUNCTION entry=lookup(ctx.Rip,&base,0);
    CHECK(entry);
    CONTEXT walk=ctx;
    unwind(0,base,walk.Rip,entry,&walk,&data,&frame,0);
    CHECK(walk.Rsp>ctx.Rsp && walk.Rip);
#else
    typedef BOOL (WINAPI *wow_context_fn)(HANDLE,void *);
    wow_context_fn wowget=(wow_context_fn)GetProcAddress(GetModuleHandleA("kernel32.dll"),"Wow64GetThreadContext");
    wow_context_fn wowset=(wow_context_fn)GetProcAddress(GetModuleHandleA("kernel32.dll"),"Wow64SetThreadContext");
    CHECK(wowget && wowset && wowget(thread,&ctx));
#endif
#ifdef _WIN64
    CHECK(ctx.Xmm7.Low == vector[0] && (unsigned long long)ctx.Xmm7.High == vector[1]);
    CHECK(ctx.MxCsr == mxcsr);
    ctx.Xmm7.Low=0x123456789abcdef0ULL;
    ctx.MxCsr=ctx.FltSave.MxCsr=0x1f80;
    CHECK(ctx.FltSave.FloatRegisters[0].Low==0x8000000000000000ULL);
    CHECK((ctx.FltSave.FloatRegisters[0].High&0xffff)==0x3fff);
    ctx.FltSave.FloatRegisters[0].High=0x4000;
    ctx.FltSave.ControlWord=0x37f;
#else
    CHECK(*(unsigned long long *)(ctx.ExtendedRegisters+160+7*16)==vector[0]);
    CHECK(*(DWORD *)(ctx.ExtendedRegisters+24)==mxcsr);
    *(unsigned long long *)(ctx.ExtendedRegisters+160+7*16)=0x123456789abcdef0ULL;
    *(DWORD *)(ctx.ExtendedRegisters+24)=0x1f80;
    CHECK(*(unsigned long long *)ctx.FloatSave.RegisterArea==0x8000000000000000ULL);
    CHECK(*(WORD *)(ctx.FloatSave.RegisterArea+8)==0x3fff);
    *(WORD *)(ctx.FloatSave.RegisterArea+8)=0x4000;
    ctx.FloatSave.ControlWord=0x37f;
#endif
    ctx.Dr0=0x12345000; ctx.Dr7=0x400;
    CHECK(SetThreadContext(thread,&ctx));
    CHECK(ntset(thread,&ctx)==0);
#ifndef _WIN64
    CHECK(wowset(thread,&ctx));
#endif
    back.ContextFlags=CONTEXT_ALL;
    CHECK(GetThreadContext(thread,&back));
    CHECK(back.Dr0==ctx.Dr0 && back.Dr7==ctx.Dr7);
#ifdef _WIN64
    CHECK(back.Xmm7.Low==ctx.Xmm7.Low && back.MxCsr==ctx.MxCsr);
    CHECK(back.FltSave.ControlWord==ctx.FltSave.ControlWord);
#else
    CHECK(*(unsigned long long *)(back.ExtendedRegisters+160+7*16)==0x123456789abcdef0ULL);
    CHECK(back.FloatSave.ControlWord==ctx.FloatSave.ControlWord);
#endif
    release_worker=1;
    CHECK(ResumeThread(thread)==1);
    CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);
    CHECK(observed_xmm[0]==0x123456789abcdef0ULL && observed_mxcsr==0x1f80);
    CHECK(*(WORD *)(observed_x87+8)==0x4000);
    CloseHandle(thread);
    return 0;
}
