// The loader's Phase 1 boot path, end to end, without default.xbe.
//
// This is the gate the plan calls "XBE mapped, 115 imports resolve, boot thread runs, first kernel
// call logged" -- run against a synthetic image instead of the real one. The loader code under test is
// the production path (parse, map, copy sections, patch fs->gs, resolve imports) and the code that runs
// afterwards is real machine code out of the fixture's .text: it calls through the import table the
// loader filled, installs an MSVC-shaped SEH frame, raises an exception and unwinds.
//
// What that does and does not prove is stated in xbe_fixture.h: the fixture is SYNTHETIC, so a green
// run means the boot path is sound, not that default.xbe would boot. Every fixture assumption gets
// checked against the real file when it arrives.
//
// Expected noise on stderr: two "[k] UNIMPLEMENTED" lines, one per stub the boot stub calls on purpose
// (ordinals 129 and 149, which the HLE layers leave unimplemented on purpose).
#include "kernel/k_memory_posix.h"
#include "kernel/k_misc_posix.h"
#include "kernel/ktable_posix.h"
#include "platform/posix/boot.h"
#include "platform/posix/image.h"
#include "platform/posix/seh.h"
#include "platform/posix/teb.h"
#include "platform/posix/xfile.h"

#include "xbe_fixture.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

using namespace bootfix;

static int g_failures, g_checks;

static void check(bool cond, const std::string &what)
{
    g_checks++;
    if (cond) {
        printf("  ok   %s\n", what.c_str());
    } else {
        printf("  FAIL %s\n", what.c_str());
        g_failures++;
    }
}

// The Phase 1 sequence, in the order the boot path runs it.
struct Boot {
    XbePosix x;
    XbeImage img;
    std::string err;

    bool prepare(bool resolve = true)
    {
        if (!x.load_from_memory(build(), err)) return false;
        if (!img.map(x, err)) return false;
        if (!img.copy_sections(err)) return false;
        img.patch_fs(nullptr);
        if (resolve && !kernel_resolve_thunks(x, img, err)) return false;
        return true;
    }
};

static uint32_t slot_at(XbeImage &img, uint32_t thunk_va, int index)
{
    const uint8_t *p = img.at(thunk_va + (uint32_t)index * 4);
    uint32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

static std::string to_hex(uint32_t v)
{
    char buf[16];
    snprintf(buf, sizeof buf, "%02x", v);
    return buf;
}

static uint32_t read_va(uint32_t va)
{
    uint32_t v = 0;
    memcpy(&v, (const void *)(uintptr_t)va, 4);
    return v;
}

// --- the import table itself --------------------------------------------------------------------------------

static void test_export_table()
{
    printf("\nkernel export table\n");

    check(strcmp(kernel_export_name(1), "AvGetSavedDataAddress") == 0, "ordinal 1 is AvGetSavedDataAddress");
    check(strcmp(kernel_export_name(302), "RtlRaiseException") == 0, "ordinal 302 is RtlRaiseException");
    check(strcmp(kernel_export_name(312), "RtlUnwind") == 0, "ordinal 312 is RtlUnwind");
    check(strcmp(kernel_export_name(16), "ExEventObjectType") == 0, "a data export is named as a data export");
    check(strcmp(kernel_export_name(9999), "?") == 0, "an unknown ordinal has no name");

    // The fixture imports exactly the set the Windows reference table knows. If either side gains or
    // loses an ordinal, this fails instead of the port quietly importing one kernel function too few.
    int unknown = 0;
    for (int i = 0; i < kKernelImportCount; i++)
        if (strcmp(kernel_export_name(kKernelOrdinals[i]), "?") == 0) unknown++;
    check(unknown == 0, "every one of the 115 fixture ordinals is in the table (" + std::to_string(unknown) + " missing)");
}

// --- resolving the table --------------------------------------------------------------------------------------

static void test_resolve()
{
    printf("\nimport resolution\n");

    {
        Boot b;
        if (!b.prepare()) {
            check(false, "boot path prepared: " + b.err);
            return;
        }

        const KernelThunkStats &st = kernel_thunk_stats();
        check(st.imports == kKernelImportCount,
              "every import filled (" + std::to_string(st.imports) + "/" + std::to_string(kKernelImportCount) + ")");
        // The implemented set grows as HLE modules land, so this says what must be in it rather than
        // counting it: the two SEH ordinals (checked by address just below) and the memory layer's.
        check(st.implemented >= 2,
              "ordinals are implemented (" + std::to_string(st.implemented) + " of " +
                  std::to_string(st.imports) + ")");
        check(st.implemented + st.data_exports + st.stubbed == st.imports, "every import is accounted for");
        // Nothing is a logging stub any more. This check was "there are some" until the DPC ordinals landed;
        // it is "there are none" now because a stub means an ordinal the game can call and get a log line
        // back from, which is worse than refusing to boot.
        check(st.stubbed == 0 && st.stub_names.empty(),
              "no ordinal is left as a logging stub" +
                  (st.stub_names.empty() ? std::string() : ": " + st.stub_names.substr(0, 60) + "..."));
        check(st.implemented + st.data_exports == st.imports,
              "every ordinal is either implemented or a data export (" + std::to_string(st.implemented) +
                  " + " + std::to_string(st.data_exports) + " = " + std::to_string(st.imports) + ")");

        int zero_slots = 0;
        for (int i = 0; i < kKernelImportCount; i++)
            if (!slot_at(b.img, THUNK_VA, i)) zero_slots++;
        check(zero_slots == 0, "no slot was left unbound");

        check(slot_at(b.img, THUNK_VA, SLOT_RAISE) == (uint32_t)(uintptr_t)&seh_raise_entry,
              "ordinal 302 points at this port's RtlRaiseException");
        check(slot_at(b.img, THUNK_VA, SLOT_UNWIND) == (uint32_t)(uintptr_t)&seh_unwind,
              "ordinal 312 points at this port's RtlUnwind");
        check(slot_at(b.img, THUNK_VA, SLOT_POOL) == (uint32_t)(uintptr_t)&ExAllocatePoolWithTag &&
                  slot_at(b.img, THUNK_VA, SLOT_NTALLOC) == (uint32_t)(uintptr_t)&NtAllocateVirtualMemory,
              "the memory layer's ordinals are bound to its implementations, not to stubs");

        // A data export must not be handed a code address: the game reads the slot and dereferences it,
        // so the slot has to hold the address of storage. It is the difference between a working XAPI
        // startup and a jump to address zero on the first read.
        const int slot_data = 6; // ordinal 16, ExEventObjectType
        uint32_t cell = slot_at(b.img, THUNK_VA, slot_data);
        check(cell == (uint32_t)(uintptr_t)ExEventObjectType,
              "a data export points at real storage, not at a thunk");
        check(cell != slot_at(b.img, THUNK_VA, SLOT_RAISE), "and that storage is not a code address");
        bool zeroed = true;
        for (size_t i = 0; i < sizeof ExEventObjectType; i++)
            if (((const uint8_t *)(uintptr_t)cell)[i]) zeroed = false;
        check(zeroed, "the object-type storage reads as zeros, as it should");
    }

    // An ordinal nobody knows must stop the boot rather than be bound to something arbitrary. This
    // needs its own mapping: only one image can own 0x10000 at a time.
    {
        std::vector<uint8_t> bytes = build();
        bootfix::put32(bytes, DATA_RAW + 3 * 4, 9999); // slot 3 now claims an ordinal that does not exist
        XbePosix x;
        XbeImage img;
        std::string err;
        bool parsed = x.load_from_memory(std::move(bytes), err);
        bool mapped = parsed && img.map(x, err);
        bool copied = mapped && img.copy_sections(err);
        bool resolved = copied && kernel_resolve_thunks(x, img, err);
        check(!resolved && err.find("9999") != std::string::npos, "an unknown ordinal is refused by name: " + err);
    }

    // The fs forms the boot stub uses must all be rewritten, or the game would read through %fs -- which
    // is base 0 here, so the chain install would fault.
    {
        Boot b;
        if (!b.prepare()) {
            check(false, "boot path prepared for the patch check: " + b.err);
            return;
        }
        const uint8_t *code = b.img.at(TEXT_VA);
        // kFsPatchOffsets is written by gen.sh from the disassembly: every instruction in the stub that
        // touches %fs, with nothing picked by hand. The earlier version listed nine offsets copied from an
        // older, shorter stub, so after any edit it was reading bytes of unrelated instructions -- and the
        // patch could regress completely while every one of those checks still passed.
        size_t gs_sites = 0;
        std::string unpatched;
        for (uint32_t off : kFsPatchOffsets) {
            if (code[off] == 0x65)
                gs_sites++;
            else
                unpatched += " 0x" + to_hex(off);
        }
        check(gs_sites == sizeof kFsPatchOffsets / sizeof kFsPatchOffsets[0],
              "every %fs instruction in the stub became %gs:" + unpatched);
        check(gs_sites >= 9, "and there are as many of them as the disassembly found (" +
                                 std::to_string(gs_sites) + ")");

        // The displacement of each patched instruction is K + the console's offset, and the console's
        // offset is what the game means: fs:[0] is the exception chain head and fs:[0xE1C] is TLS slot 3.
        uint32_t disp_head = 0, disp_tls = 0;
        bool have_head = false, have_tls = false;
        for (uint32_t off : kFsPatchOffsets) {
            uint32_t disp = 0;
            memcpy(&disp, code + off + 2, 4); // the prefix and opcode are two bytes; disp32 follows
            if (disp == teb_patch_offset(0))
                have_head = true;
            if (disp == teb_patch_offset(0xE1C))
                have_tls = true;
        }
        check(have_head, "the exception chain's displacement is K+0, this thread's TEB field");
        check(have_tls, "and the TLS slot's is K+0xE1C, slot 3 of this thread's array");
        (void)disp_head;
        (void)disp_tls;
    }
}

// --- running the boot stub -----------------------------------------------------------------------------------

static uint32_t g_boot_entry;
static uint32_t g_head_after;

static void *boot_thread(void *)
{
    ((void (*)(void))(uintptr_t)g_boot_entry)();
    g_head_after = seh_head(); // the fixture's epilogue should have put the chain back
    return nullptr;
}

static void test_boot_execution()
{
    printf("\nboot thread\n");

    Boot b;
    if (!b.prepare()) {
        check(false, "boot path prepared: " + b.err);
        return;
    }

    g_boot_entry = ENTRY_VA;
    g_head_after = 0xDEADDEADu;
    pthread_t th;
    if (pthread_create(&th, nullptr, boot_thread, nullptr) != 0) {
        check(false, "the boot thread started");
        return;
    }
    pthread_join(th, nullptr);

    check(read_va(HANDLER_RAN_VA) == 0xC0DE, "the game's own SEH frame was found by the dispatcher");
    check(read_va(HANDLER_CODE_VA) == RAISE_CODE, "the handler saw the raised exception code");
    check(read_va(HANDLER_EIP_VA) == ENTRY_VA + RAISE_RETURN_OFF,
          "Context->Eip is the address right after the call that raised it");
    check(read_va(RAISE_EAX_VA) == 1, "RtlRaiseException returned true, so the exception was handled");
    check(read_va(UNWIND_EAX_VA) == UNWIND_RV, "RtlUnwind returned the running ReturnValue");

    // Game code wrote TLS slot 3 through fs:[0xE1C] and read it back the same way. That only works if the
    // patched displacement resolves into *the thread that is running the game*, which is the whole point of
    // giving each thread its own block.
    check(read_va(TLS_SLOT_VA) == TLS_SLOT_VALUE,
          "game code read its own TLS slot back through fs:[0xE1C]");
    check(teb()->tls_slots[3] != TLS_SLOT_VALUE,
          "and that slot belongs to the boot thread, not to the thread running the test");
    check(teb()->tls_slots[0] == 0 && teb()->tls_slots[1] == 0, "the rest of this thread's slots are untouched");

    // The timer and the DPC. The boot stub armed a kernel timer from game code and spun until the DPC
    // routine -- which is also game code -- ran and recorded what it saw. Nothing else in the fixture can
    // reach the DPC worker thread, so this is the only end-to-end proof that timers fire and that a DPC runs
    // at DISPATCH_LEVEL on a thread with its own KPCR.
    check(read_va(TIMER_STATE_VA) == 1, "the boot stub armed a kernel timer (the timer path was taken)");
    check(read_va(DPC_RAN_VA) == 1, "and the DPC ran when the timer expired");
    check(read_va(DPC_IRQL_VA) == 2, "the DPC found the IRQL at DISPATCH_LEVEL (fs:[0x24] was 2)");
    check(read_va(DPC_ACTIVE_VA) == 1, "and KPRCB.DpcRoutineActive was set while it ran (fs:[0x58])");
    check(read_va(DPC_THREAD_VA) != 0, "it ran on a thread with a KTHREAD of its own (fs:[0x28])");
    check(read_va(DPC_THREAD_VA) != (uint32_t)(uintptr_t)teb()->thread,
          "and that thread is not the boot thread");
    check(read_va(DPC_ARGS_VA) != 0, "the DPC got SystemArgument1, the 100ns time the timer fired at");

    // The stub's first two calls go to real implementations now: KeRemoveQueueDpc(0) and KeSetTimer(0,0,0).
    // What they prove has not changed -- each must drop exactly its own arguments, and one of them takes a
    // LARGE_INTEGER by value, which is *two* stack slots. If the cleanup were wrong the stack would be short
    // by four or eight bytes and every check below this one would fail in a way that reads like a kernel
    // bug. So: both returned the documented "nothing to do" value, and the stub carried on working.
    check(read_va(STUB0_RET_VA) == 0, "KeRemoveQueueDpc(0) arrived with its arguments and reported nothing removed");
    check(read_va(STUB1_RET_VA) == 0,
          "and KeSetTimer(0, 0, 0) arrived with four words of arguments, including the 64-bit due time");
    check(g_head_after == EXCEPTION_CHAIN_END,
          "the game's epilogue restored the chain (head 0x" + std::to_string(g_head_after) + ")");
}

// --- the same path the program takes ---------------------------------------------------------------------
//
// The library tests above hand the parser a buffer. The program hands it a path, and that path -- open,
// seek, read, size -- had no coverage at all until now, which is the kind of gap that only shows up on
// the one file that matters.

static bool write_temp_xbe(std::string &path_out, const std::vector<uint8_t> &bytes)
{
    char tmpl[] = "/tmp/blackbootXXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return false;
    size_t done = 0;
    while (done < bytes.size()) {
        ssize_t n = write(fd, bytes.data() + done, bytes.size() - done);
        if (n <= 0) {
            close(fd);
            return false;
        }
        done += (size_t)n;
    }
    close(fd);
    path_out = tmpl;
    return true;
}

static void test_boot_from_file()
{
    printf("\nboot from a file on disk\n");

    std::string path;
    if (!write_temp_xbe(path, build())) {
        check(false, "wrote the fixture to a temporary file");
        return;
    }

    // --info: parse, report, no mapping.
    XbePosix x;
    BootReport rep;
    std::string err;
    bool ok = boot_inspect(path, x, rep, err);
    check(ok, "boot_inspect read the file: " + (ok ? std::string("ok") : err));
    if (ok) {
        check(rep.ordinals.size() == kKernelImportCount, "the import list came through the file path");
        check(rep.entry == ENTRY_VA && rep.base == BASE, "the header fields match the buffer path");
        check(rep.sections.size() == 3 && rep.sections[0].name == ".text", "the section table parsed");
        check(!rep.title.empty(), "the certificate title came through");
    }

    // --hdd has to reach the mount table. The option was parsed and then dropped on the floor for a while,
    // which no test could see because the mount table was only ever checked through its own tests.
    {
        const std::string hdd = path + "-hdd";
        XbeImage img2;
        XbePosix x2;
        BootReport rep2;
        std::string err2;
        const bool ok2 = boot_prepare(path, x2, img2, rep2, err2, hdd);
        check(ok2, "boot_prepare accepts a caller-chosen hard disk folder");
        check(xf_device_host_path("\\Device\\Harddisk0\\Partition1") == hdd + "/Partition1",
              "and that folder is what Partition1 resolves to, not the default one");
        // Only \Device\Harddisk0\Partition0..5 are mount points: \Device\Harddisk0 on its own is not
        // one, and pretending it is would hide which partition a path really lands in.
        check(xf_device_host_path("\\Device\\Harddisk0").empty() && !xf_device_host_path("\\Device\\CdRom0").empty(),
              "a device path that is not a mount point is empty, while the DVD's is not");
        if (ok2) img2.unmap();
        const std::string rm = "rm -rf '" + hdd + "'";
        if (system(rm.c_str()) != 0) printf("  (warning: could not remove %s)\n", hdd.c_str());
    }

    // The real boot sequence, then the entry point on a thread, through the same functions main() calls.
    XbeImage img;
    ok = boot_prepare(path, x, img, rep, err);
    check(ok, "boot_prepare mapped, patched and resolved: " + (ok ? std::string("ok") : err));
    if (ok) {
        check(rep.fs_patched >= 4, "the fs->gs patch ran over the mapped image (" +
                                       std::to_string(rep.fs_patched) + " sites)");
        check(rep.thunks.imports == kKernelImportCount, "every import resolved through the boot path");

        uint32_t stack_top = 0;
        const BootOutcome outcome = boot_run_entry(rep.entry, rep.stack_commit, 5000, &stack_top);
        check(outcome == BootOutcome::Returned, "the entry point returned inside the timeout");
        check(stack_top != 0, "the boot thread recorded its stack top for the crash report");
        check(read_va(HANDLER_RAN_VA) == 0xC0DE, "the same SEH round trip happened through the file path");
        check(read_va(UNWIND_EAX_VA) == UNWIND_RV, "and the unwind returned the running ReturnValue");
        check(read_va(DPC_RAN_VA) == 1 && read_va(DPC_IRQL_VA) == 2,
              "the DPC ran at DISPATCH_LEVEL through the file path as well");
    }

    // A file that is not an XBE must be refused with a reason, not half-parsed.
    {
        char tmpl[] = "/tmp/blackbootXXXXXX";
        int fd = mkstemp(tmpl);
        const char junk[] = "not an xbe at all, just some bytes to read";
        if (fd >= 0) {
            ssize_t ignored = write(fd, junk, sizeof junk - 1);
            (void)ignored;
            close(fd);
            XbePosix bad;
            BootReport badrep;
            std::string baderr;
            // Sequenced, not folded into one call: the failure text has to be read after the call, and
            // the order C++ evaluates a call's arguments is unspecified.
            const bool refused = !boot_inspect(tmpl, bad, badrep, baderr);
            check(refused && !baderr.empty(), "a non-XBE file is refused with a reason: " + baderr);
            unlink(tmpl);
        }
    }

    // A path that is not there at all: the program must say so plainly.
    {
        XbePosix missing;
        BootReport missingrep;
        std::string missingerr;
        const bool refused = !boot_inspect("/tmp/definitely-not-here.xbe", missing, missingrep, missingerr);
        check(refused && missingerr.find("cannot open") != std::string::npos,
              "a missing file is refused: " + missingerr);
    }

    unlink(path.c_str());
}

int main()
{
    printf("BLACK loader -- Phase 1 boot path (synthetic XBE)\n");

    test_export_table();
    test_resolve();
    test_boot_execution();
    test_boot_from_file();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}