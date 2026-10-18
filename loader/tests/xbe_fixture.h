// A boot-shaped synthetic XBE, for testing the loader's Phase 1 path without the real image.
//
// SYNTHETIC: everything in here is fabricated. It is not a copy of default.xbe and must not be treated
// as evidence about it. What it *is* is the same shape the loader has to handle: a header at 0x10000,
// a .text with real x86 code that reads fs-relative state and calls through the kernel import table,
// a .data holding that table, and the 115 ordinals default.xbe imports. Every assumption a test makes
// about the real image is marked SYNTHETIC below, so that the day the real XBE arrives each one can be
// checked instead of assumed.
//
// Two things here are transcribed rather than invented:
//   * the 115 ordinals (kKernelOrdinals) were extracted mechanically from the Windows reference table
//     in src/kernel/ktable.cpp, so this fixture imports exactly the set the port must resolve;
//   * the boot code (kBootCode) is the output of GNU as for the exact instruction sequence in the
//     comment above it, copied from `objdump -d -M intel`, because a hand-typed encoding that is one
//     byte wrong passes a loader test and fails on hardware.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "platform/posix/xbe.h"

namespace bootfix {

// --- image layout (mirrors what test_xbe.cpp uses, with .text/.data at the addresses the assembled
// code above hardcodes) ------------------------------------------------------------------------------------
constexpr uint32_t BASE = 0x10000;
constexpr uint32_t HEADERS = 0x1000;
constexpr uint32_t CERT_VA = BASE + 0x800;
constexpr uint32_t TLS_VA = BASE + 0x900;
constexpr uint32_t SECT_VA = BASE + 0x180;
constexpr uint32_t NAMES_VA = BASE + 0x300;
constexpr uint32_t LIBVER_VA = BASE + 0x960;
constexpr uint32_t DEBUGPATH_VA = BASE + 0xA00;

constexpr uint32_t TEXT_VA = BASE + 0xF1000; // 0x101000
constexpr uint32_t HANDLER_OFF = 0x109;       // where kBootCode's boot_handler lands -- written by gen.sh from
                                            // the disassembly, never copied by hand
constexpr uint32_t HANDLER_VA = TEXT_VA + HANDLER_OFF;
constexpr uint32_t TEXT_RAW = 0x1000;
constexpr uint32_t TEXT_SIZE = 0x1000;

constexpr uint32_t DATA_VA = BASE + 0xF2000; // 0x102000
constexpr uint32_t DATA_RAW = 0x2000;
constexpr uint32_t DATA_SIZE = 0x1000;
constexpr uint32_t DATA_VSIZE = 0x2000; // with a BSS tail, as a real .data has

constexpr uint32_t RDATA_VA = BASE + 0xF4000; // 0x104000
constexpr uint32_t RDATA_RAW = 0x3000;
constexpr uint32_t RDATA_SIZE = 0x1000;

// The image is mostly address space: .text sits at 0x101000 and .rdata ends at 0x105000, so the image
// has to be big enough to reach them. The real default.xbe is 0x487a40 for the same reason -- its
// sections are spread through a large address range, not packed at the base.
constexpr uint32_t IMAGE_SIZE = 0xF6000;

// The decoded addresses the loader will see, and the fixture's own data slots.
constexpr uint32_t THUNK_VA = DATA_VA + 0x000; // the kernel import table, 115 dwords + terminator
constexpr uint32_t RECORD_VA = DATA_VA + 0x200; // an EXCEPTION_RECORD for the boot stub to raise
constexpr uint32_t RAISE_EAX_VA = DATA_VA + 0x300; // EAX after RtlRaiseException returned
constexpr uint32_t UNWIND_EAX_VA = DATA_VA + 0x304; // EAX after RtlUnwind returned
constexpr uint32_t HANDLER_CODE_VA = DATA_VA + 0x308; // ExceptionCode the handler saw
constexpr uint32_t HANDLER_EIP_VA = DATA_VA + 0x30C; // Context->Eip the handler saw
constexpr uint32_t HANDLER_RAN_VA = DATA_VA + 0x310; // written by the handler, so "it ran" is observable
constexpr uint32_t TLS_SLOT_VA = DATA_VA + 0x314;   // what game code read back out of its own TLS slot
// The timer and the DPC the boot stub arms, and what the DPC records about the thread it ran on. Without
// these there is nothing in the whole fixture that can reach the DPC worker thread, and a kernel timer that
// is only tested by unit tests is a kernel timer whose thread state nobody has checked.
constexpr uint32_t TIMER_STATE_VA = DATA_VA + 0x318;  // 1 once the timer path was taken
constexpr uint32_t DPC_RAN_VA = DATA_VA + 0x31C;      // 1 once the DPC ran: the boot thread spins on this
constexpr uint32_t DPC_IRQL_VA = DATA_VA + 0x320;     // KPCR.Irql the DPC found at fs:[0x24]
constexpr uint32_t DPC_ACTIVE_VA = DATA_VA + 0x324;   // KPRCB.DpcRoutineActive it found at fs:[0x58]
constexpr uint32_t DPC_THREAD_VA = DATA_VA + 0x328;   // KPRCB.CurrentThread: the KTHREAD it ran on
constexpr uint32_t DPC_ARGS_VA = DATA_VA + 0x32C;     // SystemArgument1, the low word of the 100ns time
// What the two calls at the top of the stub returned. They were the only calls that used a "stub" slot, and
// with every ordinal implemented there is no stub any more -- so the return value is the evidence that the
// call arrived with its arguments where the callee expected them. A wrong stdcall cleanup leaves the stack
// short by its argument bytes, and everything the stub does afterwards then fails in a way that looks like
// a bug in the kernel rather than in the fixture's call.
constexpr uint32_t STUB0_RET_VA = DATA_VA + 0x330;     // KeRemoveQueueDpc(0): a null DPC is not queued
constexpr uint32_t STUB1_RET_VA = DATA_VA + 0x334;     // KeSetTimer(0, 0, 0): nothing was armed before
constexpr uint32_t TIMER_VA = DATA_VA + 0x400;        // the XKTIMER (40 bytes)
constexpr uint32_t DPC_OBJ_VA = DATA_VA + 0x440;      // the XKDPC object (28 bytes)
constexpr uint32_t TLS_SLOT_VALUE = 0xC0DE0003;     // the value the stub puts in TLS slot 3

// The exception the boot stub raises. SYNTHETIC: a private code, nothing in the kernel defines it, so a
// stray raise somewhere else cannot be mistaken for this one.
constexpr uint32_t RAISE_CODE = 0xE0000001u;
// What RtlUnwind is asked to return; the target is the head, so no handler may edit it.
constexpr uint32_t UNWIND_RV = 0x0000DEADu;

// The build-kind XOR keys, from xbe.cpp. The fixture is "retail".
constexpr uint32_t ENTRY_XOR_RETAIL = 0xA8FC57AB;
constexpr uint32_t THUNK_XOR_RETAIL = 0x5B6D40B6;

// The entry point is the first instruction of the boot stub. SYNTHETIC: the real XBE's entry does
// something else entirely; what matters is only that it is inside .text and XOR-encoded in the header.
constexpr uint32_t ENTRY_VA = TEXT_VA;

// ---------------------------------------------------------------------------
// The 115 ordinals default.xbe imports, ascending (the order the XDK's linker emits them in, so the
// slot index of an ordinal is its position here). Extracted from src/kernel/ktable.cpp.
// ---------------------------------------------------------------------------
constexpr uint32_t kKernelOrdinals[] = {
    1,   2,   3,   4,   8,   15,  16,  17,  23,  24,  // 0-9
    40,  44,  46,  47,  49,  67,  69,  71,  87,  95,  // 10-19
    97,  98,  99,  100, 107, 109, 113, 119, 124, 128, // 20-29
    129, 137, 139, 142, 143, 145, 149, 151, 153, 159, // 30-39
    160, 161, 164, 165, 166, 168, 171, 173, 175, 176, // 40-49
    178, 179, 180, 182, 184, 187, 189, 190, 193, 196, // 50-59
    197, 198, 199, 200, 202, 203, 207, 210, 211, 215, // 60-69
    217, 218, 219, 222, 224, 225, 226, 231, 233, 234, // 70-79
    236, 238, 246, 250, 255, 258, 259, 260, 269, 277, // 80-89
    279, 289, 291, 294, 301, 302, 304, 305, 308, 312, // 90-99
    322, 323, 324, 325, 326, 327, 328, 335, 336, 337, // 100-109
    340, 354, 356, 357, 360,                          // 110-114
};
constexpr int kKernelImportCount = (int)(sizeof(kKernelOrdinals) / sizeof(kKernelOrdinals[0]));

// Slot offsets the assembled code uses, recomputed here so the test can check the numbers rather than
// trust the transcription.
constexpr int SLOT_INITDPC = 24;  // ordinal 107, KeInitializeDpc, argc 3
constexpr int SLOT_INITEX = 26;   // ordinal 113, KeInitializeTimerEx, argc 2
constexpr int SLOT_STUB0 = 31;    // ordinal 137, KeRemoveQueueDpc, argc 1
constexpr int SLOT_STUB1 = 36;    // ordinal 149, KeSetTimer, argc 4
constexpr int SLOT_POOL = 5;      // ordinal 15,  ExAllocatePoolWithTag
constexpr int SLOT_NTALLOC = 54;  // ordinal 184, NtAllocateVirtualMemory
constexpr int SLOT_RAISE = 95;    // ordinal 302, RtlRaiseException
constexpr int SLOT_UNWIND = 99;   // ordinal 312, RtlUnwind
static_assert(kKernelOrdinals[SLOT_INITDPC] == 107, "slot 24 must be ordinal 107");
static_assert(kKernelOrdinals[SLOT_INITEX] == 113, "slot 26 must be ordinal 113");
static_assert(kKernelOrdinals[SLOT_STUB0] == 137, "slot 31 must be ordinal 137");
static_assert(kKernelOrdinals[SLOT_STUB1] == 149, "slot 36 must be ordinal 149");
static_assert(kKernelOrdinals[SLOT_POOL] == 15, "slot 5 must be ordinal 15");
static_assert(kKernelOrdinals[SLOT_NTALLOC] == 184, "slot 54 must be ordinal 184");
static_assert(kKernelOrdinals[SLOT_RAISE] == 302, "slot 95 must be ordinal 302");
static_assert(kKernelOrdinals[SLOT_UNWIND] == 312, "slot 99 must be ordinal 312");

// Where the RtlRaiseException call returns to: the instruction right after `call eax`, from the objdump
// below. The handler must report exactly this as Context->Eip (the contract in seh.h), so the test asserts
// it -- which is why these offsets are read from the disassembly rather than counted by hand.
// These three are measured by gen.sh and written here, because the stub moves whenever anything ahead of them
// changes size and a hand-copied offset was stale every single time one was pasted in.
constexpr uint32_t RAISE_RETURN_OFF = 0xA3;             // the instruction right after the call that raised
constexpr uint32_t PROLOGUE_PUSH_HANDLER_OFF = 0x84;    // where the prologue pushes its exception handler
constexpr uint32_t kFsPatchOffsets[] = {
    0x6E,       // mov    DWORD PTR fs:0xe1c,0xc0de0003
    0x79,       // mov    eax,fs:0xe1c
    0x89,       // push   DWORD PTR fs:0x0
    0x90,       // mov    DWORD PTR fs:0x0,esp
    0xA8,       // mov    eax,fs:0x0
    0xC7,       // mov    DWORD PTR fs:0x0,ecx
    0xD2,       // mov    eax,fs:0x24
    0xDD,       // mov    eax,fs:0x58
    0xE8,       // mov    eax,fs:0x28
};


constexpr uint32_t RAISE_PUSH_RECORD_OFF = 0x97;        // where the raise pushes its exception record
constexpr uint32_t THUNK_READ_STUB0_OFF = 0x02;         // mov eax, [thunk slot 31]: ordinal 137
constexpr uint32_t THUNK_READ_STUB1_OFF = 0x16;         // mov eax, [thunk slot 36]: ordinal 149

// ---------------------------------------------------------------------------
// The boot stub, as GNU as emitted it for this sequence (Intel syntax source):
//
//   boot_entry:                             ; entered by a host thread, returns to it
//       mov eax, dword ptr [THUNK_VA]        ; slot 0: ordinal 1
//       call eax
//       push 0 / push 0 / push 0 / push 0   ; ordinal 2 takes four arguments
//       mov eax, dword ptr [THUNK_VA+4]      ; slot 1: ordinal 2
//       call eax
//       mov dword ptr fs:[0xE1C], TLS_SLOT_VALUE   ; TLS slot 3, written through fs
//       mov eax, fs:[0xE1C]                 ; and read back through the same addressing
//       mov dword ptr [OUT_TLS], eax         ; left where the test can see what came back
//       push HANDLER                        ; the handler below, which sits at +0x7C
//       push dword ptr fs:[0]               ; the old head
//       mov dword ptr fs:[0], esp           ; install -- the game's own SEH frame
//       push RECORD                         ; &record
//       mov eax, dword ptr [THUNK_VA+95*4]  ; slot 95: ordinal 302
//       call eax
//       mov dword ptr [OUT_RAISE], eax      ; EAX after the raise
//       mov eax, dword ptr fs:[0]           ; the frame just installed
//       push 0xdead / push 0 / push 0 / push eax     ; stdcall order, so the callee reads
//                                                   ; target frame, ip, record, return value
//       mov eax, dword ptr [THUNK_VA+99*4]  ; slot 99: ordinal 312
//       call eax
//       mov dword ptr [OUT_UNWIND], eax     ; EAX after the unwind
//       mov ecx, dword ptr [esp+4]          ; the old head, where the prologue left it
//       mov dword ptr fs:[0], ecx           ; restore the chain
//       add esp, 8
//       ret
//   boot_handler:                            ; long handler(record, establisher, context, dispatcher)
//       mov eax, dword ptr [esp+4] / mov eax, dword ptr [eax] / mov dword ptr [OUT_HCODE], eax
//       mov eax, dword ptr [esp+12] / mov eax, dword ptr [eax+0xB8] / mov dword ptr [OUT_HEIP], eax
//         (the arguments are record, establisher frame, context, dispatcher -- [esp+8] is the frame,
//          not the context, which is the kind of off-by-one that writes a plausible wrong number)
//       mov dword ptr [OUT_HRAN], 0xC0DE     ; "the handler ran"
//       xor eax, eax                        ; ExceptionContinueExecution
//       ret
//
// Note the fs-relative forms it uses -- `push dword [fs:0]`, `mov [fs:0], esp`, `mov eax, [fs:0]`,
// `mov dword [fs:0], imm32` and `mov eax, [fs:0xE1C]` -- because each is a different encoding for the
// patcher to recognise, the `mov [fs:0], esp` is what an MSVC try/except prologue actually emits, and the
// 0xE1C pair is the TLS slot the game's CRT reaches for.
// ---------------------------------------------------------------------------
constexpr uint8_t kBootCode[] = {
    0x6A, 0x00,                                  // 00  push   0x0
    0xA1, 0x7C, 0x20, 0x10, 0x00,                // 02  mov    eax,ds:0x10207c
    0xFF, 0xD0,                                  // 07  call   eax
    0xA3, 0x30, 0x23, 0x10, 0x00,                // 09  mov    ds:0x102330,eax
    0x6A, 0x00,                                  // 0E  push   0x0
    0x6A, 0x00,                                  // 10  push   0x0
    0x6A, 0x00,                                  // 12  push   0x0
    0x6A, 0x00,                                  // 14  push   0x0
    0xA1, 0x90, 0x20, 0x10, 0x00,                // 16  mov    eax,ds:0x102090
    0xFF, 0xD0,                                  // 1B  call   eax
    0xA3, 0x34, 0x23, 0x10, 0x00,                // 1D  mov    ds:0x102334,eax
    0xC7, 0x05, 0x18, 0x23, 0x10, 0x00, 0x01, 0x00, 0x00, 0x00, // 22  mov    DWORD PTR ds:0x102318,0x1
    0x6A, 0x01,                                  // 2C  push   0x1
    0x68, 0x00, 0x24, 0x10, 0x00,                // 2E  push   0x102400
    0xA1, 0x68, 0x20, 0x10, 0x00,                // 33  mov    eax,ds:0x102068
    0xFF, 0xD0,                                  // 38  call   eax
    0x6A, 0x00,                                  // 3A  push   0x0
    0x68, 0xD2, 0x10, 0x10, 0x00,                // 3C  push   0x1010d2
    0x68, 0x40, 0x24, 0x10, 0x00,                // 41  push   0x102440
    0xA1, 0x60, 0x20, 0x10, 0x00,                // 46  mov    eax,ds:0x102060
    0xFF, 0xD0,                                  // 4B  call   eax
    0x68, 0x40, 0x24, 0x10, 0x00,                // 4D  push   0x102440
    0x6A, 0x00,                                  // 52  push   0x0
    0x68, 0xC0, 0xF2, 0xFC, 0xFF,                // 54  push   0xfffcf2c0
    0x68, 0x00, 0x24, 0x10, 0x00,                // 59  push   0x102400
    0xA1, 0x90, 0x20, 0x10, 0x00,                // 5E  mov    eax,ds:0x102090
    0xFF, 0xD0,                                  // 63  call   eax
    0x83, 0x3D, 0x1C, 0x23, 0x10, 0x00, 0x00,    // 65  cmp    DWORD PTR ds:0x10231c,0x0
    0x74, 0xF7,                                  // 6C  je     0x65
    0x64, 0xC7, 0x05, 0x1C, 0x0E, 0x00, 0x00, 0x03, 0x00, 0xDE, 0xC0, // 6E  mov    DWORD PTR fs:0xe1c,0xc0de0003
    0x64, 0xA1, 0x1C, 0x0E, 0x00, 0x00,          // 79  mov    eax,fs:0xe1c
    0xA3, 0x14, 0x23, 0x10, 0x00,                // 7F  mov    ds:0x102314,eax
    0x68, 0x09, 0x11, 0x10, 0x00,                // 84  push   0x101109
    0x64, 0xFF, 0x35, 0x00, 0x00, 0x00, 0x00,    // 89  push   DWORD PTR fs:0x0
    0x64, 0x89, 0x25, 0x00, 0x00, 0x00, 0x00,    // 90  mov    DWORD PTR fs:0x0,esp
    0x68, 0x00, 0x22, 0x10, 0x00,                // 97  push   0x102200
    0xA1, 0x7C, 0x21, 0x10, 0x00,                // 9C  mov    eax,ds:0x10217c
    0xFF, 0xD0,                                  // A1  call   eax
    0xA3, 0x00, 0x23, 0x10, 0x00,                // A3  mov    ds:0x102300,eax
    0x64, 0xA1, 0x00, 0x00, 0x00, 0x00,          // A8  mov    eax,fs:0x0
    0x68, 0xAD, 0xDE, 0x00, 0x00,                // AE  push   0xdead
    0x6A, 0x00,                                  // B3  push   0x0
    0x6A, 0x00,                                  // B5  push   0x0
    0x50,                                        // B7  push   eax
    0xA1, 0x8C, 0x21, 0x10, 0x00,                // B8  mov    eax,ds:0x10218c
    0xFF, 0xD0,                                  // BD  call   eax
    0xA3, 0x04, 0x23, 0x10, 0x00,                // BF  mov    ds:0x102304,eax
    0x8B, 0x0C, 0x24,                            // C4  mov    ecx,DWORD PTR [esp]
    0x64, 0x89, 0x0D, 0x00, 0x00, 0x00, 0x00,    // C7  mov    DWORD PTR fs:0x0,ecx
    0x83, 0xC4, 0x08,                            // CE  add    esp,0x8
    0xC3,                                        // D1  ret
    0x64, 0xA1, 0x24, 0x00, 0x00, 0x00,          // D2  mov    eax,fs:0x24
    0xA3, 0x20, 0x23, 0x10, 0x00,                // D8  mov    ds:0x102320,eax
    0x64, 0xA1, 0x58, 0x00, 0x00, 0x00,          // DD  mov    eax,fs:0x58
    0xA3, 0x24, 0x23, 0x10, 0x00,                // E3  mov    ds:0x102324,eax
    0x64, 0xA1, 0x28, 0x00, 0x00, 0x00,          // E8  mov    eax,fs:0x28
    0xA3, 0x28, 0x23, 0x10, 0x00,                // EE  mov    ds:0x102328,eax
    0x8B, 0x44, 0x24, 0x0C,                      // F3  mov    eax,DWORD PTR [esp+0xc]
    0xA3, 0x2C, 0x23, 0x10, 0x00,                // F7  mov    ds:0x10232c,eax
    0xC7, 0x05, 0x1C, 0x23, 0x10, 0x00, 0x01, 0x00, 0x00, 0x00, // FC  mov    DWORD PTR ds:0x10231c,0x1
    0xC2, 0x10, 0x00,                            // 106  ret    0x10
    0x8B, 0x44, 0x24, 0x04,                      // 109  mov    eax,DWORD PTR [esp+0x4]  (boot_handler)
    0x8B, 0x00,                                  // 10D  mov    eax,DWORD PTR [eax]
    0xA3, 0x08, 0x23, 0x10, 0x00,                // 10F  mov    ds:0x102308,eax
    0x8B, 0x44, 0x24, 0x0C,                      // 114  mov    eax,DWORD PTR [esp+0xc]
    0x8B, 0x80, 0xB8, 0x00, 0x00, 0x00,          // 118  mov    eax,DWORD PTR [eax+0xb8]
    0xA3, 0x0C, 0x23, 0x10, 0x00,                // 11E  mov    ds:0x10230c,eax
    0xC7, 0x05, 0x10, 0x23, 0x10, 0x00, 0xDE, 0xC0, 0x00, 0x00, // 123  mov    DWORD PTR ds:0x102310,0xc0de
    0x31, 0xC0,                                  // 12D  xor    eax,eax
    0xC3,                                        // 12F  ret
};












constexpr uint32_t BOOT_ENTRY_LEN = 0x109; // boot_handler starts here
constexpr uint32_t DPC_OFF = 0xD2;       // boot_dpc: the deferred procedure the timer runs

// The immediates below must agree with the constants they were generated from, or the stub silently
// reads the wrong memory -- the section table lives at 0x10180 and looks enough like code to call.
#ifndef BOOTFIX_SKIP_ASM_ASSERTS // see tests/fixture_gen/gen.sh, which checks this against objdump
static_assert(sizeof kBootCode == BOOT_ENTRY_LEN + 0x27, "the handler follows the entry at 0xE9 and runs to 0x110");
#endif
constexpr uint32_t STUB0_VA = THUNK_VA + SLOT_STUB0 * 4;
constexpr uint32_t STUB1_VA = THUNK_VA + SLOT_STUB1 * 4;   // also where the boot stub arms the timer
constexpr uint32_t INITDPC_VA = THUNK_VA + SLOT_INITDPC * 4;
constexpr uint32_t INITEX_VA = THUNK_VA + SLOT_INITEX * 4;
#ifndef BOOTFIX_SKIP_ASM_ASSERTS // see tests/fixture_gen/gen.sh
// These four checks compare the low byte and the third byte of each immediate -- the third is the one
// that moves when the fixture's layout moves. The top byte of every address here is zero, so checking it
// proves nothing; an earlier version of these asserts did exactly that and passed forever.
static_assert(kBootCode[THUNK_READ_STUB0_OFF] == 0xA1 && kBootCode[THUNK_READ_STUB0_OFF + 1] == (uint8_t)(STUB0_VA & 0xFF) &&
                      kBootCode[THUNK_READ_STUB0_OFF + 3] == (uint8_t)((STUB0_VA >> 16) & 0xFF),
                  "boot_entry reads the thunk slot for ordinal 137");
static_assert(kBootCode[THUNK_READ_STUB1_OFF] == 0xA1 && kBootCode[THUNK_READ_STUB1_OFF + 1] == (uint8_t)(STUB1_VA & 0xFF) &&
                      kBootCode[THUNK_READ_STUB1_OFF + 3] == (uint8_t)((STUB1_VA >> 16) & 0xFF),
                  "and then the one for ordinal 149");
#endif
#ifndef BOOTFIX_SKIP_ASM_ASSERTS
static_assert(kBootCode[PROLOGUE_PUSH_HANDLER_OFF] == 0x68 &&
                      kBootCode[PROLOGUE_PUSH_HANDLER_OFF + 1] == (uint8_t)(HANDLER_VA & 0xFF) &&
                      kBootCode[PROLOGUE_PUSH_HANDLER_OFF + 3] == (uint8_t)((HANDLER_VA >> 16) & 0xFF),
                  "the prologue pushes the handler that actually exists in this image");
static_assert(kBootCode[RAISE_PUSH_RECORD_OFF] == 0x68 &&
                      kBootCode[RAISE_PUSH_RECORD_OFF + 1] == (uint8_t)(RECORD_VA & 0xFF) &&
                      kBootCode[RAISE_PUSH_RECORD_OFF + 3] == (uint8_t)((RECORD_VA >> 16) & 0xFF),
                  "the raise passes the record that lives in .data");
static_assert(kBootCode[RAISE_RETURN_OFF] != 0, "the address after the raising call is a real instruction");
static_assert(kBootCode[DPC_OFF] == 0x64 && kBootCode[DPC_OFF + 1] == 0xA1 && kBootCode[DPC_OFF + 2] == 0x24,
              "boot_dpc starts with its first fs read, which is what makes it a deferred procedure");
static_assert(kBootCode[HANDLER_OFF] == 0x8B && kBootCode[HANDLER_OFF + 1] == 0x44,
              "boot_handler is where the header says");
#endif

// --- little writers that refuse to run off the end of the buffer ---------------------------------------

inline void put32(std::vector<uint8_t> &d, size_t off, uint32_t v)
{
    if (off + 4 > d.size()) { fprintf(stderr, "FATAL: put32 at 0x%zx past the 0x%zx-byte fixture\n", off, d.size()); abort(); }
    memcpy(&d[off], &v, 4);
}

inline void put16(std::vector<uint8_t> &d, size_t off, uint16_t v)
{
    if (off + 2 > d.size()) { fprintf(stderr, "FATAL: put16 at 0x%zx past the 0x%zx-byte fixture\n", off, d.size()); abort(); }
    memcpy(&d[off], &v, 2);
}

inline void put_bytes(std::vector<uint8_t> &d, size_t off, const void *p, size_t n)
{
    if (off + n > d.size()) { fprintf(stderr, "FATAL: put_bytes at 0x%zx (%zu) past the 0x%zx-byte fixture\n", off, n, d.size()); abort(); }
    memcpy(&d[off], p, n);
}

inline void add_section(std::vector<uint8_t> &d, size_t idx, uint32_t flags, uint32_t vaddr, uint32_t vsize,
                         uint32_t raw_addr, uint32_t raw_size, const char *name, uint32_t names_off)
{
    size_t o = (SECT_VA - BASE) + idx * 56;
    XbeSectionHeader sh{};
    sh.flags = flags;
    sh.virtual_addr = vaddr;
    sh.virtual_size = vsize;
    sh.raw_addr = raw_addr;
    sh.raw_size = raw_size;
    sh.name_addr = NAMES_VA + names_off;
    sh.reference_count = 1;
    put_bytes(d, o, &sh, sizeof sh);
    put_bytes(d, (NAMES_VA + names_off) - BASE, name, strlen(name) + 1);
}

// The image. SYNTHETIC throughout; see the top of this file.
inline std::vector<uint8_t> build()
{
    std::vector<uint8_t> d(0x4000, 0);
    put_bytes(d, 0, "XBEH", 4);

    put32(d, 0x104, BASE);
    put32(d, 0x108, HEADERS);
    put32(d, 0x10C, IMAGE_SIZE);
    put32(d, 0x114, 1033924800u);
    put32(d, 0x118, CERT_VA);
    put32(d, 0x11C, 3); // three sections
    put32(d, 0x120, SECT_VA);
    put32(d, 0x128, ENTRY_VA ^ ENTRY_XOR_RETAIL);
    put32(d, 0x12C, TLS_VA);
    put32(d, 0x130, 0x10000); // stack commit
    put32(d, 0x134, 0x10000);
    put32(d, 0x138, 0x10000);
    put32(d, 0x13C, BASE);
    put32(d, 0x140, IMAGE_SIZE);
    put32(d, 0x14C, DEBUGPATH_VA);
    put32(d, 0x158, THUNK_VA ^ THUNK_XOR_RETAIL);
    put32(d, 0x160, 1); // one library
    put32(d, 0x164, LIBVER_VA);

    // Certificate: title id, media, region, ratings. The boot path never reads them, but a real image
    // has them and the parser does look.
    put32(d, (CERT_VA - BASE) + 4, 1033924800u);
    put32(d, (CERT_VA - BASE) + 8, 0x43484350u);
    const char16_t title[] = u"BLACK BOOT FIXTURE";
    for (int i = 0; title[i]; i++) put16(d, (CERT_VA - BASE) + 0x0C + i * 2, (uint16_t)title[i]);
    put32(d, (CERT_VA - BASE) + 0x9C, 0x03);
    put32(d, (CERT_VA - BASE) + 0xA0, 0x01);
    put32(d, (CERT_VA - BASE) + 0xA4, 0xFF);

    // TLS directory: six dwords. SYNTHETIC -- the real image's TLS block belongs to the game's CRT,
    // and its shape is one of the things that has to be checked against the real file.
    put32(d, (TLS_VA - BASE) + 0, TEXT_VA + 0x70);
    put32(d, (TLS_VA - BASE) + 4, TEXT_VA + 0x78);
    put32(d, (TLS_VA - BASE) + 8, DATA_VA + 0x800);
    put32(d, (TLS_VA - BASE) + 12, DATA_VA + 0x900);
    put32(d, (TLS_VA - BASE) + 16, 0x1000);
    put32(d, (TLS_VA - BASE) + 20, 0x100000);

    const char *debug_path = "unit:\\boot\\default.xbe";
    put_bytes(d, DEBUGPATH_VA - BASE, debug_path, strlen(debug_path) + 1);

    add_section(d, 0, XBE_SEC_EXECUTABLE | XBE_SEC_PRELOAD, TEXT_VA, TEXT_SIZE, TEXT_RAW, TEXT_SIZE, ".text", 0);
    add_section(d, 1, XBE_SEC_WRITABLE | XBE_SEC_PRELOAD, DATA_VA, DATA_VSIZE, DATA_RAW, DATA_SIZE, ".data", 0x10);
    add_section(d, 2, XBE_SEC_EXECUTABLE, RDATA_VA, RDATA_SIZE, RDATA_RAW, RDATA_SIZE, ".rdata", 0x18);

    put_bytes(d, TEXT_RAW, kBootCode, sizeof kBootCode);

    // The kernel import table: 115 ordinals, then the zero terminator.
    for (int i = 0; i < kKernelImportCount; i++) put32(d, DATA_RAW + (uint32_t)i * 4, kKernelOrdinals[i]);
    put32(d, DATA_RAW + (uint32_t)kKernelImportCount * 4, 0);

    // The EXCEPTION_RECORD the boot stub raises, living at RECORD_VA inside .data.
    const size_t rec_file = DATA_RAW + (RECORD_VA - DATA_VA);
    put32(d, rec_file + 0x00, RAISE_CODE);      // ExceptionCode
    put32(d, rec_file + 0x04, 0);               // ExceptionFlags
    put32(d, rec_file + 0x0C, ENTRY_VA + 0x35); // ExceptionAddress: where the raise call returns
    put32(d, rec_file + 0x10, 1);               // NumberParameters
    put32(d, rec_file + 0x14, 0x0000BEEF);      // ExceptionInformation[0]

    return d;
}

} // namespace bootfix