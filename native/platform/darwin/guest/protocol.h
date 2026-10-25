// The control block the macOS host shares with its guest agent. The host (arm64) and the agent (x86_64, under Rosetta 2) both compile
// this; every field has a fixed width and offset on either side.
//
// The service channel carries memory, mapping, descriptor and pthread operations, which a thread of the agent
// performs whenever the host asks, whether Windows code is running or not. Each thread channel belongs to one guest pthread: when Windows
// code stops (a breakpoint, a fault, a system call, an interrupt) the agent's signal handler publishes the registers there and waits; the
// host reads and changes them in place and lets the thread resume, and the handler returns into them.
#pragma once
#include <stdint.h>

#define RAVEN_GUEST_MAGIC 0x52564e47U // "RVNG"
#define RAVEN_GUEST_VERSION 4U

// Descriptors the host gives the agent at startup.
#define RAVEN_GUEST_CONTROL_FD 3
#define RAVEN_GUEST_SOCKET_FD 4
#define RAVEN_GUEST_WAKE_FD 5
#define RAVEN_GUEST_MAX_THREADS 256

// The XSAVE layout the runtime keeps extended state in: the legacy FXSAVE image, the XSAVE header, and the upper halves of YMM0-15.
#define RAVEN_XSTATE_SIZE 832
#define RAVEN_XSTATE_HEADER 512
#define RAVEN_XSTATE_YMM 576

// 32-bit code runs through LDT descriptors (Rosetta accepts indices from 7 on): the code segment, then one data segment per TEB.
#define RAVEN_LDT_CODE32 7
#define RAVEN_LDT_FIRST_TEB 8
#define RAVEN_LDT_SELECTOR(index) ((uint32_t)(index) * 8 + 7)
#define RAVEN_MAC_CS64 0x2b

// The low 4 GiB, held by a zero-fill segment of the agent until the host maps memory there.
#define RAVEN_LOW_START 0x10000ULL
#define RAVEN_LOW_END 0xffff0000ULL

enum raven_service_command {
	RAVEN_SERVICE_NONE,
	// a0 address (0: anywhere), a1 size, a2 protection, a3 flags: anonymous private memory.
	RAVEN_SERVICE_MAP,
	// a0 address, a1 size, a2 protection.
	RAVEN_SERVICE_PROTECT,
	// a0 address, a1 size: the range goes back to the low reserve when it lies in it.
	RAVEN_SERVICE_UNMAP,
	// a0 address, a1 size, a2 protection, a3 1 when the range is a shared view: private pages read as zero afterwards.
	RAVEN_SERVICE_DISCARD,
	// The descriptor sent on the socket: a0 address (0: anywhere), a1 size, a2 offset, a3 flags, a4 protection.
	RAVEN_SERVICE_MAP_DESCRIPTOR,
	// a0 index, a1 base, a2 limit (bytes), a3 1 for a 32-bit code segment.
	RAVEN_SERVICE_SET_LDT,
	// a0 address, a1 size: guest memory into the data area.
	RAVEN_SERVICE_READ,
	// a0 address, a1 size, a2 protection the pages have: the data area into guest memory, through the guest's own stores.
	RAVEN_SERVICE_WRITE,
	// a0 address, a1 size: result 1 when nothing is mapped there.
	RAVEN_SERVICE_AVAILABLE,
	// a0 address, a1 size: the address range code was changed in, so it is translated again.
	RAVEN_SERVICE_FLUSH,
	// a0 channel index: start a pthread and wait for its initial stop, interrupt it, or stop and join it.
	RAVEN_SERVICE_THREAD_CREATE,
	RAVEN_SERVICE_THREAD_INTERRUPT,
	RAVEN_SERVICE_THREAD_JOIN,
	// a0 address, a1 expected, a2 desired, a3 operand size (4 or 8): a locked exchange in the guest's memory domain.
	RAVEN_SERVICE_COMPARE_EXCHANGE,
	// data area: NUL-terminated host path; a0 function count. Result: __wine_unix_call_funcs table in this process.
	RAVEN_SERVICE_LOAD_UNIXLIB,
	RAVEN_SERVICE_EXIT,
};

enum raven_map_flags {
	// Exactly at the address and only where nothing is mapped (MAP_FIXED_NOREPLACE).
	RAVEN_MAP_EXACT = 1,
	// Below 4 GiB, for 32-bit programs.
	RAVEN_MAP_LOW = 2,
	// On a 64 KiB boundary, the Windows allocation granularity.
	RAVEN_MAP_GRANULAR = 4,
};

enum raven_thread_state {
	RAVEN_THREAD_RUNNING,
	RAVEN_THREAD_STOPPED,
	RAVEN_THREAD_RESUME,
	RAVEN_THREAD_EXIT,
};

// Stop reasons beyond signal numbers.
#define RAVEN_STOP_INTERRUPT (-1)

#define RAVEN_UNIX_EVENT_SLOTS 64
#define RAVEN_SURFACE_SLOTS 16

enum raven_surface_state {
	RAVEN_SURFACE_UNUSED,
	RAVEN_SURFACE_AVAILABLE,
	RAVEN_SURFACE_READY,
	RAVEN_SURFACE_DISPLAYED,
	RAVEN_SURFACE_WRITING,
	RAVEN_SURFACE_RETIRE,
	RAVEN_SURFACE_IMPORTING,
};

// A guest-owned IOSurface send right. The host copies it through the guest task port; no global IOSurface IDs are used.
struct raven_surface_frame {
	uint32_t state;
	uint32_t port;
	uint64_t window;
	uint64_t sequence;
	uint32_t width, height;
	uint32_t pixel_format;
	uint32_t reserved;
};

// Linux's user_regs_struct, field for field.
struct raven_registers {
	uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8, rax, rcx, rdx, rsi, rdi, orig_rax, rip, cs, eflags, rsp, ss, fs_base,
		gs_base, ds, es, fs, gs;
};

struct raven_service_channel {
	uint32_t request;
	uint32_t reply;
	uint32_t command;
	int32_t error;
	uint64_t arguments[8];
	uint64_t result;
	uint64_t reserved[5];
};

struct raven_thread_channel {
	uint32_t state;
	// Interrupts the host asks for and those the agent has answered: a stop of any kind answers every interrupt asked for before it.
	uint32_t interrupts_requested;
	uint32_t interrupts_answered;
	int32_t signal;
	int32_t code;
	uint32_t trace;
	uint64_t fault_address;
	uint32_t unix_active;
	uint32_t reserved0;
	uint64_t reserved[3];
	struct raven_registers registers;
	uint8_t xstate[RAVEN_XSTATE_SIZE];
};

struct raven_control {
	uint32_t magic;
	uint32_t version;
	uint32_t ready;
	uint32_t parallel;
	// Filled in by the agent: where calls return to (an INT3) and its process identity.
	uint64_t return_gate;
	uint64_t agent_pid;
	uint64_t unix_call_gate;
	uint64_t unix_call_gate_end;
	uint64_t reserved[10];
	struct raven_service_channel service;
	uint64_t pad1[2];
	struct raven_thread_channel threads[RAVEN_GUEST_MAX_THREADS];
	uint64_t pad2[6];
	// Native GPU completion callbacks publish Windows event handles; the host scheduler consumes each nonzero slot.
	uint64_t unix_events[RAVEN_UNIX_EVENT_SLOTS];
	struct raven_surface_frame surfaces[RAVEN_SURFACE_SLOTS];
	uint64_t surfaces_presented;
	uint64_t surface_errors;
};

// The size of the shared file: the block, and the data area that READ and WRITE move memory through.
#define RAVEN_CONTROL_SIZE (1024 * 1024)
#define RAVEN_CONTROL_DATA_OFFSET (512 * 1024)
#define RAVEN_CONTROL_DATA_SIZE (RAVEN_CONTROL_SIZE - RAVEN_CONTROL_DATA_OFFSET)

#ifdef __cplusplus
static_assert(sizeof(struct raven_registers) == 27 * 8, "Linux register layout");
static_assert(sizeof(struct raven_control) <= RAVEN_CONTROL_DATA_OFFSET, "control block fits before data area");
#else
_Static_assert(sizeof(struct raven_registers) == 27 * 8, "Linux register layout");
_Static_assert(sizeof(struct raven_control) <= RAVEN_CONTROL_DATA_OFFSET, "control block fits before data area");
#endif
