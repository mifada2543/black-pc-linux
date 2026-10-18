// Structured exception handling for game code, on a host that has none of its own.
//
// The problem
// -----------
// The XBE was built with MSVC, so its try/except compiles to explicit manipulation of the fs:[0]
// chain head. On Windows the loader simply bound ordinal 302 (RtlRaiseException) and 312
// (RtlUnwind) to host ntdll (ktable.cpp), so the game's frames and the host's frames shared one
// chain. Linux has no such thing: a fault arrives as a signal, and %fs is not even ours to point at
// a TEB (see teb.h). So the chain is ours to own, and the two ordinals have to be implemented here.
//
// What the game emits, and what we must honour
// --------------------------------------------
// MSVC installs a handler by pushing the handler, then the old chain head, then storing esp into
// fs:[0]:
//
//     push  handler
//     push  dword [fs:0]
//     mov   dword [fs:0], esp
//
// so the registration record is { prev, handler } -- the link at +0, the handler at +4 -- and fs:[0]
// points at the newest record. That is why EstablisherFrame is simply the record address, and it is
// the opposite of the { ExceptionHandler, Next } order the kernel-mode struct documents, which is
// the kind of detail that costs a day when it is guessed. The evidence:
//   * Wine's i386 nested-frame helper (dlls/ntdll/signal_i386.c, call_exception_handler) pushes the
//     handler and then %fs:0, installs with `mov %fs:0, %esp` and *unlinks* with `mov %fs:0, %esp` --
//     i.e. the chain link is the first dword of the record;
//   * ReactOS's ntdll does the same thing in C: RtlUnwind unlinks a frame with RtlpSetExceptionList,
//     whose i386 body is `mov ecx,[ecx]; mov fs:[TEB_EXCEPTION_LIST], ecx` -- again, record[0].
// The synthetic boot fixture in tests/xbe_fixture.h is the same prologue, and reading it the other way
// round makes the dispatcher call the previous chain head as if it were a handler.
//
// Dispatch walks the chain from the head, calling each handler with the 4-argument prototype and the
// NT disposition set. A chain that runs out is the signal that the fault was nobody's fault, and the
// caller reports it and lets the process die.
//
// A catch does not come back through the dispatcher. MSVC's _except_handler3, when a filter says
// EXCEPTION_EXECUTE_HANDLER, calls RtlUnwind (ordinal 312, seh_unwind) and then non-locally jumps
// into the __except block; the loader's dispatch frames -- like the kernel dispatcher's frames on
// Windows -- are simply abandoned, and game execution continues. Everything the loader does around
// handler calls is therefore kept to plain stack locals that nobody needs to unwind.
//
// Deliberate simplifications, recorded in .agents/plan.md:
//   - a handler that returns ExceptionContinueExecution on a NONCONTINUABLE record, or any handler
//     returning a disposition outside the legal set, is reported and treated as unhandled; NT would
//     raise STATUS_NONCONTINUABLE_EXCEPTION / STATUS_INVALID_DISPOSITION as a fresh exception and
//     search the chain again (risk 1b)
//   - the nested-exception trampoline around handler calls (NT's RtlpExecuteHandlerForException
//     with its raise_handler) is not reproduced: a fault inside a handler re-enters the signal
//     bridge as an ordinary nested dispatch instead
//   - floating-point and vector register state in CONTEXT is zeroed rather than captured, because
//     nothing in the image currently reads it
#pragma once

#include <stddef.h>
#include <stdint.h>

#define EXCEPTION_MAXIMUM_PARAMETERS 15

// Windows CONTEXT for x86. Field order and offsets are the documented ones (win32 CONTEXT_i386),
// because game handlers compiled by MSVC read this struct by offset, not by name. The signal bridge
// fills the integer, control and segment registers and claims CONTEXT_FULL; the float save and the
// extended registers stay zero (recorded as a deliberate simplification in the header note above).
struct FloatSave {
    uint32_t control_word;     // 0x00
    uint32_t status_word;      // 0x04
    uint32_t tag_word;         // 0x08
    uint32_t error_offset;     // 0x0C
    uint32_t error_selector;   // 0x10
    uint32_t data_offset;      // 0x14
    uint32_t data_selector;    // 0x18
    uint8_t register_area[80]; // 0x1C
    uint32_t cr0_npx_state;    // 0x6C
};
static_assert(sizeof(FloatSave) == 112, "FLOAT_SAVE must be 112 bytes");

struct Context {
    uint32_t context_flags;             // 0x000
    uint32_t dr0, dr1, dr2, dr3, dr6, dr7; // 0x004..0x01B
    FloatSave float_save;               // 0x01C
    uint32_t seg_gs, seg_fs, seg_es, seg_ds; // 0x08C..0x09B
    uint32_t edi, esi, ebx, edx, ecx, eax; // 0x09C..0x0B3
    uint32_t ebp, eip, seg_cs, eflags, esp, seg_ss; // 0x0B4..0x0CB
    uint8_t extended_registers[512];    // 0x0CC
};
static_assert(sizeof(Context) == 0x2CC, "CONTEXT must be 0x2CC bytes (x86)");
static_assert(offsetof(Context, float_save) == 0x1C, "FloatSave sits at 0x1C");
static_assert(offsetof(Context, seg_gs) == 0x8C, "SegGs sits at 0x8C");
static_assert(offsetof(Context, edi) == 0x9C, "Edi sits at 0x9C");
static_assert(offsetof(Context, eax) == 0xB0, "Eax sits at 0xB0");
static_assert(offsetof(Context, ebp) == 0xB4, "Ebp sits at 0xB4");
static_assert(offsetof(Context, eip) == 0xB8, "Eip sits at 0xB8");
static_assert(offsetof(Context, esp) == 0xC4, "Esp sits at 0xC4");
static_assert(offsetof(Context, extended_registers) == 0xCC, "ExtendedRegisters sits at 0xCC");

// Windows EXCEPTION_RECORD for x86: 80 bytes. There is no __unusedAlignment field here -- that field
// exists only to align ExceptionInformation to 8 bytes on x64, where the struct is 152 bytes.
// Offsets matter because MSVC-compiled game handlers read this struct by offset.
struct ExceptionRecord {
    uint32_t exception_code;               // 0x00
    uint32_t exception_flags;              // 0x04
    struct ExceptionRecord *exception_record; // 0x08
    void *exception_address;               // 0x0C
    uint32_t number_parameters;            // 0x10
    uint32_t exception_information[EXCEPTION_MAXIMUM_PARAMETERS]; // 0x14, ULONG_PTR on x86
};
static_assert(sizeof(ExceptionRecord) == 80, "EXCEPTION_RECORD must be 80 bytes (x86)");
static_assert(offsetof(ExceptionRecord, exception_address) == 0x0C, "ExceptionAddress sits at 0x0C");
static_assert(offsetof(ExceptionRecord, exception_information) == 0x14,
              "ExceptionInformation sits at 0x14 on x86");

// One link of the chain. Must match the layout the game's prologue builds: prev (the old chain head)
// first, handler at +4 -- see the note at the top of this file.
struct ExceptionRegistration {
    struct ExceptionRegistration *prev;
    void *handler;
};
static_assert(sizeof(ExceptionRegistration) == 8, "registration record must be 8 bytes");
static_assert(offsetof(ExceptionRegistration, prev) == 0, "the chain link sits at 0x00");
static_assert(offsetof(ExceptionRegistration, handler) == 4, "the handler sits at 0x04");

// Handler return values: the NT EXCEPTION_DISPOSITION set, which is what the frame handlers the
// game's CRT installs (_except_handler3 and friends) return. NOT the filter vocabulary
// (EXCEPTION_EXECUTE_HANDLER / EXCEPTION_CONTINUE_SEARCH / EXCEPTION_CONTINUE_EXECUTION) that
// __except(...) expressions return to _except_handler3 -- that one never crosses this boundary,
// because the loader calls frame handlers, not filters.
enum {
    ExceptionContinueExecution = 0, // the handler edited *context: resume it
    ExceptionContinueSearch = 1,    // not this frame: try the next (older) one
    ExceptionNestedException = 2,   // NT nested-dispatch bookkeeping; reported, not implemented
    ExceptionCollidedUnwind = 3,    // legal only while the chain is being unwound; see seh_unwind
};

// Exception record flags.
enum {
    EXCEPTION_NONCONTINUABLE = 0x01u,
    EXCEPTION_UNWINDING = 0x02u,
    EXCEPTION_EXIT_UNWIND = 0x04u,
};

// Exception codes we produce or care about.
enum {
    EXCEPTION_ACCESS_VIOLATION = 0xC0000005u,
    EXCEPTION_ILLEGAL_INSTRUCTION = 0xC000001Du,
    EXCEPTION_STACK_OVERFLOW = 0xC00000FDu,
    EXCEPTION_INT_DIVIDE_BY_ZERO = 0xC0000094u,
    EXCEPTION_BREAKPOINT = 0x80000003u,
    // Xbox ordinal 312/302 take these; the values are the ones the game compares against.
    XBOX_STATUS_NO_MEMORY = 0xC0000017u,
    STATUS_INVALID_UNWIND_TARGET = 0xC0000023u,
    STATUS_INVALID_DISPOSITION = 0xC0000026u,
    STATUS_UNWIND = 0xC0000027u,
};

// The chain ends here on NT (ExceptionList is ~0 when empty, and the loader's TEB starts at it). The
// game's prologues inherit whatever word they find, so a walk must treat both ~0 and NULL as the
// end; the unit tests cover both.
static constexpr uint32_t EXCEPTION_CHAIN_END = 0xFFFFFFFFu;

// The handler prototype MSVC code expects.
using SehHandler = long (*)(ExceptionRecord *record, void *establisher_frame, Context *context, void *dispatcher);

// This thread's chain head. The loader reads it through teb()->seh; the game's rewritten fs:[0]
// reads the same word. Kept as a function so seh.h has no dependency on teb.h internals.
uint32_t seh_head();

// Dispatch record shared with the handlers we run.
struct SehDispatch {
    ExceptionRecord *record;
    Context *context;
    uint32_t code;
    // Filled when a handler returned ExceptionContinueExecution: the context to resume from. NT
    // resumes from the CONTEXT as the handler edited it, never from record->exception_address.
    Context *resume_context;
    // Set when the chain ran out, or when a handler misbehaved and a diagnostic was printed.
    bool unhandled;
};

// Run the chain (first pass). Returns ExceptionContinueExecution when a handler asked to resume
// (resume_context is set), or ExceptionContinueSearch when it was nobody's: unhandled is set, and so
// is it for the reported-instead-of-raised misbehaviours described at the top of this file.
long seh_dispatch(ExceptionRecord *record, Context *context, SehDispatch *out);

// Raise as the Xbox ordinal 302 would, and report whether some handler took responsibility. Walks the
// chain; on ExceptionContinueExecution the caller must resume from *context as the handler left it.
// This is the internal helper: it takes a context the caller supplies.
bool seh_raise(ExceptionRecord *record, Context *context);

// The ordinal 302 entry point itself -- NTSTATUS RtlRaiseException(PEXCEPTION_RECORD). __stdcall,
// because the game pushes the argument and the callee has to pop it: binding a cdecl function here
// leaves every raise four bytes off the game's stack. The boot test found that the hard way (the entry
// returned into the address of the frame handler). The context it dispatches with is the caller's own,
// captured the same way seh_unwind does.
extern "C" __attribute__((stdcall)) uint32_t seh_raise_entry(ExceptionRecord *record);

// Unwind as the Xbox ordinal 312 (ntdll!RtlUnwind) would. The i386 contract below is the one NT's
// own RtlUnwind implements (WRK rtl/i386/exdsptch.c) and Wine's ntdll reproduces; Wine's test suite,
// which runs on Windows, pins the observable parts:
//
//   RtlUnwind(TargetFrame, TargetIp, Record, ReturnValue);   // four stack arguments, EAX back
//
//   - Callers see a NORMAL RETURN. EAX carries ReturnValue, after each unwind handler has had a
//     chance to edit Context->Eax (the suite pins that: a handler that adds 1 turns 0xDEAD0000 into
//     a returned 0xDEAD0001).
//   - TargetIp is ignored on i386; NT's own docs say so ("ignored if the target frame parameter is
//     not specified" -- and in practice MSVC's CRT passes the label right after the call, which is
//     where a normal return lands anyway).
//   - Record == NULL builds a local record: code STATUS_UNWIND, ExceptionAddress = this call's
//     return address, no parameters. That is the common path, not an edge case: the CRT's
//     _global_unwind2 calls RtlUnwind(frame, label, NULL, 0).
//   - The record (the caller's, or the local one) gets EXCEPTION_UNWINDING set, plus
//     EXCEPTION_EXIT_UNWIND when TargetFrame is NULL.
//   - Every frame strictly newer than TargetFrame has its handler called in unwind mode and is then
//     unlinked; TargetFrame itself stays installed and stops the walk. Newer frames sit at lower
//     addresses, so a frame above TargetFrame means the target is not in the chain, which NT raises
//     STATUS_INVALID_UNWIND_TARGET for; reported here instead, see the notes at the top.
//   - TargetFrame == 0xFFFFFFFF is the CRT's "unwind everything" epilogue form: every frame is
//     unwound and the call returns normally. (NT special-cases exactly this value.) A NULL
//     TargetFrame is NT's exit unwind: the walk is identical, but NT then re-raises the record
//     instead of returning -- patched to a plain return here, see seh.cpp, because nothing in the
//     XBE's CRT reaches that path and re-raising needs machinery that does not exist yet.
//   - Unwind handlers must answer ExceptionContinueSearch. ExceptionCollidedUnwind means the walk
//     collided with a local unwind already in progress (the CRT's __unwind_handler protector): the
//     handler stores the frame to continue from through its dispatcher argument, that frame is
//     unlinked without being called, and the walk resumes at its prev, exactly as NT's loop does.
//   - Handlers see a context with Eip = this call's return address (pinned as well), Eax = the
//     running ReturnValue, and Esp/Ebp as the caller will have them once this returns.
//
// This IS the ordinal 312 entry point, not a helper: it takes the four arguments in the Xbox order,
// is __stdcall so a binding may point the game's thunk straight at its address (the way Windows
// pointed 312 at ntdll), and its caller-frame arithmetic (the return address, the caller's Esp/Ebp)
// is only correct when the direct caller is the game's CRT code. C linkage for the same reason: the
// unit tests call it from hand-written assembly, standing in for the CRT's call site.
extern "C" __attribute__((stdcall)) uint32_t seh_unwind(void *target_frame, void *target_ip,
                                                        ExceptionRecord *record, uint32_t return_value);

// Install the signal bridge so that a fault inside game code becomes an EXCEPTION_RECORD and goes
// through the chain instead of killing the process. Call once, after the image is mapped.
void seh_install_signal_bridge();

// A breakpoint the loader put there on purpose. Returns the previous probe, so a caller can put it back.
//
// This exists for one case: a library function whose entry is shorter than the five bytes a jump needs
// cannot be patched with one, so it gets a single 0xCC instead and the trap is recognised here. The
// Windows port does the same thing with a vectored exception handler (dispatch.cpp:int3_handler); without
// this, SIGTRAP keeps its default action and the run dies with "trace/breakpoint trap" instead of the
// report that says which library function was reached.
using BreakpointProbe = bool (*)(uint32_t eip, uint32_t eax, uint32_t ecx, uint32_t edx,
                                const uint32_t *frame);
BreakpointProbe seh_set_breakpoint_probe(BreakpointProbe probe);

// Report an unhandled exception the way hooks.cpp's crash reporter does, then let the process die.
void seh_fatal(const ExceptionRecord *record, const Context *context);