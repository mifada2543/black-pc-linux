// Kernel export table for Linux: which Xbox kernel import lands where, and the logging stub for
// everything not implemented yet.
//
// This is the POSIX half of loader/src/kernel/ktable.cpp, and the Windows file stays the reference:
// the ordinals, names, calling conventions and argument counts below were extracted from it
// mechanically, because the import table is a contract with the game. An ordinal that is missing, or
// whose argument count is wrong, does not fail here -- it leaves the game's stack off by four bytes
// somewhere much later.
//
// Where the Windows port binds a real implementation, this one binds the logging stub, with two
// exceptions: ordinals 302 and 312 are the SEH machinery this port implements itself (seh.h), because
// the game and the loader must share one exception chain -- there is no second chain to hand them.
//
// The stub is the Windows design unchanged: a 16-byte thunk per unbound ordinal
//
//     push <ordinal>
//     jmp  stub_common
//
// and one shared tail that logs the call, counts it, and drops the number of argument bytes the Xbox
// ABI says the call occupies. Without that last part an unimplemented call corrupts the stack instead
// of merely returning garbage.
//
// Data exports (the V entries -- XeImageFileName, XboxHDKey, the object-type pointers) are not calls:
// the game reads the slot's value and dereferences it. Binding one to a stub would hand the game a
// code address to dereference, so each gets a real zero-filled cell instead, which kernel_bind_variable
// can later point at something meaningful.
#include "kernel/ktable_posix.h"

#include "platform/posix/image.h"
#include "platform/posix/seh.h"

#include <stdio.h>
#include <string.h>

#include <atomic>
#include <string>

#include <sys/mman.h>

// The tail every generated thunk jumps to; defined in assembly further down.
extern "C" void stub_common();

namespace {

enum KConv : uint8_t { K_STD, K_FAST, K_CDECL, K_VAR };

struct KExport {
    uint16_t ordinal;
    const char *name;
    KConv conv;
    uint8_t argc;   // stack + register arguments, which is what the stub's stack cleanup needs
    void *impl;     // nullptr = the logging stub, or a variable cell for K_VAR
};

// Every ordinal default.xbe imports (115), ascending, with the argument counts from the Xbox kernel
// ABI. Generated from src/kernel/ktable.cpp on 2026-10-17; do not retype these.
KExport kExports[] = {
    {1, "AvGetSavedDataAddress", K_STD, 0, nullptr},
    {2, "AvSendTVEncoderOption", K_STD, 4, nullptr},
    {3, "AvSetDisplayMode", K_STD, 6, nullptr},
    {4, "AvSetSavedDataAddress", K_STD, 1, nullptr},
    {8, "DbgPrint", K_CDECL, 0, nullptr},
    {15, "ExAllocatePoolWithTag", K_STD, 2, nullptr},
    {16, "ExEventObjectType", K_VAR, 0, nullptr},
    {17, "ExFreePool", K_STD, 1, nullptr},
    {23, "ExQueryPoolBlockSize", K_STD, 1, nullptr},
    {24, "ExQueryNonVolatileSetting", K_STD, 5, nullptr},
    {40, "HalDiskCachePartitionCount", K_VAR, 0, nullptr},
    {44, "HalGetInterruptVector", K_STD, 2, nullptr},
    {46, "HalReadWritePCISpace", K_STD, 6, nullptr},
    {47, "HalRegisterShutdownNotification", K_STD, 2, nullptr},
    {49, "HalReturnToFirmware", K_STD, 1, nullptr},
    {67, "IoCreateSymbolicLink", K_STD, 2, nullptr},
    {69, "IoDeleteSymbolicLink", K_STD, 1, nullptr},
    {71, "IoFileObjectType", K_VAR, 0, nullptr},
    {87, "IofCompleteRequest", K_FAST, 2, nullptr},
    {95, "KeBugCheck", K_STD, 1, nullptr},
    {97, "KeCancelTimer", K_STD, 1, nullptr},
    {98, "KeConnectInterrupt", K_STD, 1, nullptr},
    {99, "KeDelayExecutionThread", K_STD, 3, nullptr},
    {100, "KeDisconnectInterrupt", K_STD, 1, nullptr},
    {107, "KeInitializeDpc", K_STD, 3, nullptr},
    {109, "KeInitializeInterrupt", K_STD, 7, nullptr},
    {113, "KeInitializeTimerEx", K_STD, 2, nullptr},
    {119, "KeInsertQueueDpc", K_STD, 3, nullptr},
    {124, "KeQueryBasePriorityThread", K_STD, 1, nullptr},
    {128, "KeQuerySystemTime", K_STD, 1, nullptr},
    {129, "KeRaiseIrqlToDpcLevel", K_STD, 0, nullptr},
    {137, "KeRemoveQueueDpc", K_STD, 1, nullptr},
    {139, "KeRestoreFloatingPointState", K_STD, 1, nullptr},
    {142, "KeSaveFloatingPointState", K_STD, 1, nullptr},
    {143, "KeSetBasePriorityThread", K_STD, 2, nullptr},
    {145, "KeSetEvent", K_STD, 3, nullptr},
    {149, "KeSetTimer", K_STD, 4, nullptr},
    {151, "KeStallExecutionProcessor", K_STD, 1, nullptr},
    {153, "KeSynchronizeExecution", K_STD, 3, nullptr},
    {159, "KeWaitForSingleObject", K_STD, 5, nullptr},
    {160, "KfRaiseIrql", K_FAST, 1, nullptr},
    {161, "KfLowerIrql", K_FAST, 1, nullptr},
    {164, "LaunchDataPage", K_VAR, 0, nullptr},
    {165, "MmAllocateContiguousMemory", K_STD, 1, nullptr},
    {166, "MmAllocateContiguousMemoryEx", K_STD, 5, nullptr},
    {168, "MmClaimGpuInstanceMemory", K_STD, 2, nullptr},
    {171, "MmFreeContiguousMemory", K_STD, 1, nullptr},
    {173, "MmGetPhysicalAddress", K_STD, 1, nullptr},
    {175, "MmLockUnlockBufferPages", K_STD, 3, nullptr},
    {176, "MmLockUnlockPhysicalPage", K_STD, 2, nullptr},
    {178, "MmPersistContiguousMemory", K_STD, 3, nullptr},
    {179, "MmQueryAddressProtect", K_STD, 1, nullptr},
    {180, "MmQueryAllocationSize", K_STD, 1, nullptr},
    {182, "MmSetAddressProtect", K_STD, 3, nullptr},
    {184, "NtAllocateVirtualMemory", K_STD, 5, nullptr},
    {187, "NtClose", K_STD, 1, nullptr},
    {189, "NtCreateEvent", K_STD, 4, nullptr},
    {190, "NtCreateFile", K_STD, 9, nullptr},
    {193, "NtCreateSemaphore", K_STD, 4, nullptr},
    {196, "NtDeviceIoControlFile", K_STD, 10, nullptr},
    {197, "NtDuplicateObject", K_STD, 3, nullptr},
    {198, "NtFlushBuffersFile", K_STD, 2, nullptr},
    {199, "NtFreeVirtualMemory", K_STD, 3, nullptr},
    {200, "NtFsControlFile", K_STD, 10, nullptr},
    {202, "NtOpenFile", K_STD, 6, nullptr},
    {203, "NtOpenSymbolicLinkObject", K_STD, 2, nullptr},
    {207, "NtQueryDirectoryFile", K_STD, 10, nullptr},
    {210, "NtQueryFullAttributesFile", K_STD, 2, nullptr},
    {211, "NtQueryInformationFile", K_STD, 5, nullptr},
    {215, "NtQuerySymbolicLinkObject", K_STD, 3, nullptr},
    {217, "NtQueryVirtualMemory", K_STD, 2, nullptr},
    {218, "NtQueryVolumeInformationFile", K_STD, 5, nullptr},
    {219, "NtReadFile", K_STD, 8, nullptr},
    {222, "NtReleaseSemaphore", K_STD, 3, nullptr},
    {224, "NtResumeThread", K_STD, 2, nullptr},
    {225, "NtSetEvent", K_STD, 2, nullptr},
    {226, "NtSetInformationFile", K_STD, 5, nullptr},
    {231, "NtSuspendThread", K_STD, 2, nullptr},
    {233, "NtWaitForSingleObject", K_STD, 3, nullptr},
    {234, "NtWaitForSingleObjectEx", K_STD, 4, nullptr},
    {236, "NtWriteFile", K_STD, 8, nullptr},
    {238, "NtYieldExecution", K_STD, 0, nullptr},
    {246, "ObReferenceObjectByHandle", K_STD, 3, nullptr},
    {250, "ObfDereferenceObject", K_FAST, 1, nullptr},
    {255, "PsCreateSystemThreadEx", K_STD, 10, nullptr},
    {258, "PsTerminateSystemThread", K_STD, 1, nullptr},
    {259, "PsThreadObjectType", K_VAR, 0, nullptr},
    {260, "RtlAnsiStringToUnicodeString", K_STD, 3, nullptr},
    {269, "RtlCompareMemoryUlong", K_STD, 3, nullptr},
    {277, "RtlEnterCriticalSection", K_STD, 1, nullptr},
    {279, "RtlEqualString", K_STD, 3, nullptr},
    {289, "RtlInitAnsiString", K_STD, 2, nullptr},
    {291, "RtlInitializeCriticalSection", K_STD, 1, nullptr},
    {294, "RtlLeaveCriticalSection", K_STD, 1, nullptr},
    {301, "RtlNtStatusToDosError", K_STD, 1, nullptr},
    {302, "RtlRaiseException", K_STD, 1, nullptr},
    {304, "RtlTimeFieldsToTime", K_STD, 2, nullptr},
    {305, "RtlTimeToTimeFields", K_STD, 2, nullptr},
    {308, "RtlUnicodeStringToAnsiString", K_STD, 3, nullptr},
    {312, "RtlUnwind", K_STD, 4, nullptr},
    {322, "XboxHardwareInfo", K_VAR, 0, nullptr},
    {323, "XboxHDKey", K_VAR, 0, nullptr},
    {324, "XboxKrnlVersion", K_VAR, 0, nullptr},
    {325, "XboxSignatureKey", K_VAR, 0, nullptr},
    {326, "XeImageFileName", K_VAR, 0, nullptr},
    {327, "XeLoadSection", K_STD, 1, nullptr},
    {328, "XeUnloadSection", K_STD, 1, nullptr},
    {335, "XcSHAInit", K_STD, 1, nullptr},
    {336, "XcSHAUpdate", K_STD, 3, nullptr},
    {337, "XcSHAFinal", K_STD, 2, nullptr},
    {340, "XcHMAC", K_STD, 7, nullptr},
    {354, "XboxAlternateSignatureKeys", K_VAR, 0, nullptr},
    {356, "HalBootSMCVideoMode", K_VAR, 0, nullptr},
    {357, "IdexChannelObject", K_VAR, 0, nullptr},
    {360, "HalInitiateShutdown", K_STD, 0, nullptr},
};

KExport *find_export(uint32_t ordinal)
{
    for (auto &e : kExports)
        if (e.ordinal == ordinal) return &e;
    return nullptr;
}

// Function pointers to void *, the way the thunk table stores them. The intermediate hop through
// uintptr_t keeps it to one reinterpret_cast.
template <typename Fn> void *fn_ptr(Fn f) { return reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(f)); }

std::atomic<uint32_t> g_stub_calls[400];
bool g_trace;

// One RWX page for generated thunks, and one RW page for data-export cells.
constexpr size_t PAGE = 0x10000;
uint8_t *g_stub_page;
size_t g_stub_used;
uint8_t *g_var_page;
size_t g_var_used;
void **g_var_slot[400]; // ordinal -> its cell, so kernel_bind_variable() can fill it in later

void *make_stub(uint32_t ordinal)
{
    if (!g_stub_page) {
        void *p = mmap(nullptr, PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return nullptr;
        g_stub_page = (uint8_t *)p;
    }
    if (g_stub_used + 16 > PAGE) return nullptr;
    uint8_t *p = g_stub_page + g_stub_used;
    g_stub_used += 16;
    p[0] = 0x68; // push imm32
    memcpy(p + 1, &ordinal, 4);
    p[5] = 0xE9; // jmp rel32
    int32_t rel = (int32_t)((uintptr_t)&stub_common - (uintptr_t)(p + 10));
    memcpy(p + 6, &rel, 4);
    return p;
}

void *variable_slot(uint32_t ordinal)
{
    if (!g_var_page) {
        void *p = mmap(nullptr, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return nullptr;
        g_var_page = (uint8_t *)p; // MAP_ANONYMOUS pages read as zero
    }
    if (g_var_used + 4 > PAGE) return nullptr;
    void **slot = (void **)(g_var_page + g_var_used);
    g_var_used += 4;
    *slot = nullptr;
    if (ordinal < 400) g_var_slot[ordinal] = slot;
    return slot;
}

KernelThunkStats g_stats;

} // namespace

// Called from the generated thunk, with the ordinal it was built for. cdecl on purpose: the thunk
// restores the stack itself, from the byte count returned in edx.
extern "C" uint64_t blackrt_stub_called_posix(uint32_t ordinal, const uint32_t *args, uint32_t ecx, uint32_t edx)
{
    const KExport *e = find_export(ordinal);
    uint32_t n = ordinal < 400 ? ++g_stub_calls[ordinal] : 1;
    char argbuf[256] = "";
    int pos = 0;
    if (e) {
        for (uint32_t i = 0; i < e->argc && pos < 200; i++) {
            uint32_t v = (e->conv == K_FAST && i == 0)   ? ecx
                         : (e->conv == K_FAST && i == 1) ? edx
                                                          : args[e->conv == K_FAST ? i - 2 : i];
            pos += snprintf(argbuf + pos, sizeof argbuf - pos, "%s0x%x", i ? ", " : "", v);
        }
    }
    if (n == 1 || g_trace)
        fprintf(stderr, "[k] UNIMPLEMENTED %s(%s) ordinal %u, call #%u\n", e ? e->name : "?", argbuf, ordinal, n);

    // What the caller expects to find dropped from its stack when it returns.
    uint32_t bytes = 0;
    if (e) {
        if (e->conv == K_STD) bytes = e->argc * 4;
        else if (e->conv == K_FAST && e->argc > 2) bytes = (e->argc - 2) * 4;
    }
    return ((uint64_t)bytes << 32) | 0u;
}

// The shared tail of every generated thunk. On entry the stack holds [esp] = ordinal, [esp+4] = return
// address, [esp+8] = the call's arguments -- the ordinal was pushed by the stub, so the arguments have
// moved up by four. This mirrors the __declspec(naked) stub_common in the Windows ktable.cpp; GCC has
// no naked functions on x86, so it is written as a file-scope asm block (a .S file would need
// enable_language(ASM) for one function -- see .agents/plan.md risk 5 for the eleven hook thunks that
// do need it). The definition lives further down.
__asm__(".text\n"
        ".globl stub_common\n"
        ".type stub_common, @function\n"
        "stub_common:\n"
        "    pushl %ebp\n"
        "    movl %esp, %ebp\n"        // [ebp+4] ordinal, [ebp+8] return address, [ebp+12] arguments
        "    pushl %edx\n"            // fastcall arguments arrive in these two registers, so keep them
        "    pushl %ecx\n"
        "    leal 12(%ebp), %eax\n"   // &arguments
        "    pushl %eax\n"
        "    pushl 4(%ebp)\n"        // ordinal
        "    call blackrt_stub_called_posix\n" // eax = result, edx = bytes of arguments to drop
        "    movl %ebp, %esp\n"
        "    popl %ebp\n"             // [esp] ordinal, [esp+4] return address
        "    movl 4(%esp), %ecx\n"    // return to the game
        "    leal 8(%esp,%edx), %esp\n" // past the ordinal, the return address and the arguments
        "    jmp *%ecx\n"
        ".size stub_common, .-stub_common\n");

const char *kernel_export_name(uint32_t ordinal)
{
    const KExport *e = find_export(ordinal);
    return e ? e->name : "?";
}

bool kernel_bind_export(uint32_t ordinal, void *target)
{
    KExport *e = find_export(ordinal);
    if (!e) return false;
    e->impl = target;
    return true;
}

bool kernel_bind_variable(uint32_t ordinal, void *address)
{
    if (ordinal < 400 && g_var_slot[ordinal]) {
        *g_var_slot[ordinal] = address;
        return true;
    }
    // Not resolved yet: remember it so the cell gets the value when the thunk table is filled.
    return kernel_bind_export(ordinal, address);
}

uint32_t kernel_stub_calls(uint32_t ordinal) { return ordinal < 400 ? g_stub_calls[ordinal].load() : 0; }

void kernel_set_trace(bool on) { g_trace = on; }

const KernelThunkStats &kernel_thunk_stats() { return g_stats; }

bool kernel_resolve_thunks(const XbePosix &x, XbeImage &img, std::string &err)
{
    g_stats = KernelThunkStats();
    if (!x.thunk_addr) {
        err = "the image has no kernel import table";
        return false;
    }

    // A run of 32-bit slots terminated by a zero entry. Bounded by the image size so a truncated table
    // cannot walk into the next section looking for the terminator.
    const uint32_t max_slots = x.size_of_image / 4 + 1;
    for (uint32_t i = 0, va = x.thunk_addr; i < max_slots; i++, va += 4) {
        uint8_t *slot = img.at(va);
        if (!slot) {
            err = "thunk slot " + std::to_string(i) + " at 0x" + std::to_string(va) + " is outside the mapped image";
            return false;
        }
        uint32_t raw;
        memcpy(&raw, slot, 4);
        if (!raw) break; // terminator

        const uint32_t ordinal = raw & 0x7FFFFFFF; // the top bit is a flag, not part of the ordinal
        const KExport *e = find_export(ordinal);
        if (!e) {
            err = "ordinal " + std::to_string(ordinal) + " (slot " + std::to_string(i) +
                  ") is not in the export table; add it to ktable_posix.cpp";
            return false;
        }

        void *target = e->impl;
        if (ordinal == 302) target = fn_ptr(&seh_raise_entry); // this port's own SEH: one shared chain
        else if (ordinal == 312) target = fn_ptr(&seh_unwind);
        if (!target && e->conv == K_VAR) target = variable_slot(ordinal);

        if (!target) {
            target = make_stub(ordinal);
            g_stats.stubbed++;
            if (!g_stats.stub_names.empty()) g_stats.stub_names += ", ";
            g_stats.stub_names += e->name;
        } else if (e->conv == K_VAR) {
            g_stats.data_exports++;
        } else {
            g_stats.implemented++;
        }
        if (!target) {
            err = "cannot build a thunk for ordinal " + std::to_string(ordinal) + " (" + e->name + ")";
            return false;
        }

        const uint32_t addr = (uint32_t)(uintptr_t)target;
        memcpy(slot, &addr, 4);
        g_stats.imports++;
    }
    if (!g_stats.imports) {
        err = "the kernel import table is empty";
        return false;
    }
    return true;
}