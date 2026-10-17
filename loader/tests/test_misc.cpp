// The miscellaneous kernel exports on POSIX: AV, HAL, interrupts, non-volatile settings, XBE sections,
// crypto, the data exports, DbgPrint's format translation, and the three calls that end the run.
//
// Three things in here are worth explaining, because they are the parts that can pass while being wrong:
//
//   * DbgPrint's format translation is tested against the MSVC spellings directly, and end to end
//     through the value DbgPrint returns. "%I64x" reaching glibc unchanged does not crash; it prints
//     the literal text and misreads the argument, which is the kind of bug that reaches the game
//     unnoticed if nothing checks it.
//   * XeLoadSection runs against the synthetic XBE fixture, so it copies real section bytes to a real
//     mapped address and the reference count in the image is the one the game would read.
//   * KeBugCheck and HalReturnToFirmware must never return. They are checked in a forked child with
//     its stderr captured, because a bugcheck that returned would take this whole suite down with it
//     and because the log line saying which code was bugchecked is worth asserting on.
#include "kernel/k_misc_posix.h"
#include "platform/posix/boot.h"
#include "platform/posix/image.h"
#include "platform/posix/log.h"
#include "platform/posix/xbe.h"

#include "xbe_fixture.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

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

static void check_u32(uint32_t got, uint32_t want, const std::string &what)
{
    g_checks++;
    if (got == want) {
        printf("  ok   %s\n", what.c_str());
    } else {
        printf("  FAIL %s (got 0x%08x, want 0x%08x)\n", what.c_str(), got, want);
        g_failures++;
    }
}

// The translator, compared as a string so a failure shows both sides.
static void check_format(const char *in, const char *want, const std::string &what)
{
    char out[256];
    const size_t n = kmisc_translate_format(in, out, sizeof out);
    g_checks++;
    if (strcmp(out, want) == 0 && n == strlen(want)) {
        printf("  ok   %s\n", what.c_str());
    } else {
        printf("  FAIL %s (\"%s\" -> \"%s\", want \"%s\")\n", what.c_str(), in, out, want);
        g_failures++;
    }
}

static std::string to_hex(const uint8_t *p, size_t n)
{
    static const char *d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s += d[p[i] >> 4];
        s += d[p[i] & 0xF];
    }
    return s;
}

// ----------------------------------------------------------------------------------------------------------
// DbgPrint
// ----------------------------------------------------------------------------------------------------------

static void test_dbgprint_format()
{
    printf("\nDbgPrint formats\n");

    // Formats that need no translation must come out untouched, including the ones whose % is not a
    // conversion at all.
    check_format("plain text", "plain text", "text passes through");
    check_format("%d %s %x %c", "%d %s %x %c", "the common conversions are left alone");
    check_format("100%% done", "100%% done", "a literal percent survives");
    check_format("%08X %-5d %.3f", "%08X %-5d %.3f", "flags, widths and precision are left alone");
    check_format("%ls %lc", "%ls %lc", "glibc's own wide modifiers are left alone");

    // The MSVC spellings glibc does not know.
    check_format("%I64x", "%llx", "%I64x becomes %llx");
    check_format("%I64d", "%lld", "%I64d becomes %lld");
    check_format("%08I64X", "%08llX", "a width in front of %I64X keeps its place");
    check_format("%I32d", "%d", "%I32d needs no length modifier");
    check_format("%S", "%ls", "%S becomes %ls");
    check_format("%C", "%lc", "%C becomes %lc");
    check_format("%p", "%08X", "%p becomes eight uppercase hex digits, like MSVC");
    check_format("value=%I64x of %S", "value=%llx of %ls", "a format with several directives");

    // Truncation has snprintf's semantics: the return value is what the result would have been, so a
    // caller can tell that a format did not fit.
    char small[8];
    const size_t n = kmisc_translate_format("%I64x and more text", small, sizeof small);
    check(n == strlen("%llx and more text") && strlen(small) == sizeof small - 1,
          "a short buffer truncates but still reports the full length");

    // End to end: the return value is what a game that stores the result sees, and it is computed from
    // the translated format's output. A 16-hex-digit value cannot be produced by the literal text.
    const uint32_t len = DbgPrint("misc test: %I64x\n", (uint64_t)0x1122334455667788ULL);
    check_u32(len, strlen("misc test: 1122334455667788"),
              "DbgPrint formats %I64x correctly and returns the character count");
    check_u32(DbgPrint("no newline"), strlen("no newline"), "a format without a newline is not padded with one");
    check_u32(DbgPrint("%s", "string arg"), strlen("string arg"), "a string argument still works");
    check_u32(DbgPrint("%d%%", 50), strlen("50%"), "a percent next to a conversion is not doubled");
}

// ----------------------------------------------------------------------------------------------------------
// non-volatile settings
// ----------------------------------------------------------------------------------------------------------

static void test_settings()
{
    printf("\nnon-volatile settings\n");

    uint32_t type = 0, result_len = 0, value = 0xDEADBEEF;

    check(ExQueryNonVolatileSetting(0x007, &type, &value, sizeof value, &result_len) == XF_SUCCESS,
          "XC_LANGUAGE is readable");
    check_u32(value, 1, "and answers English, which is what the game branches on");
    check_u32(type, XREG_DWORD, "a setting that is a DWORD says so");
    check_u32(result_len, 4, "and reports its length");

    check(ExQueryNonVolatileSetting(0x012, &type, &value, sizeof value, &result_len) == XF_SUCCESS &&
              value == 1,
          "XC_DVD_REGION is region 1");

    kmisc_set_widescreen(false);
    check(ExQueryNonVolatileSetting(0x008, &type, &value, sizeof value, &result_len) == XF_SUCCESS && value == 0,
          "XC_VIDEO is 4:3 by default");
    kmisc_set_widescreen(true);
    check(ExQueryNonVolatileSetting(0x008, &type, &value, sizeof value, &result_len) == XF_SUCCESS &&
              value == 0x00010000,
          "XC_VIDEO reports the widescreen TV bit when asked for it");
    kmisc_set_widescreen(false);

    check(ExQueryNonVolatileSetting(0x103, &type, &value, sizeof value, &result_len) == XF_SUCCESS &&
              value == 0x00400100,
          "XC_FACTORY_AV_REGION answers NTSC-M at 60 Hz");

    // A binary section: the name/date settings, and the whole-block read the dashboard uses.
    uint8_t block[256];
    memset(block, 0xCC, sizeof block);
    check(ExQueryNonVolatileSetting(0x0FF, &type, block, sizeof block, &result_len) == XF_SUCCESS,
          "a whole settings section is readable");
    check_u32(type, XREG_BINARY, "and is reported as binary");
    check_u32(result_len, 256, "with the length that was asked for");
    bool zeros = true;
    for (size_t i = 0; i < result_len; i++)
        if (block[i]) zeros = false;
    check(zeros, "and is filled with zeros, as an unwritten console has it");

    // The failure paths, because a game that gets XF_SUCCESS with a short buffer would read past the end.
    check(ExQueryNonVolatileSetting(0x0FFF, &type, &value, sizeof value, &result_len) == XF_OBJECT_NAME_NOT_FOUND,
          "an index that is not a setting says so instead of answering");
    check(ExQueryNonVolatileSetting(0x007, &type, &value, 2, &result_len) == XF_BUFFER_TOO_SMALL,
          "a buffer too small for the value is refused");
    check(ExQueryNonVolatileSetting(0x007, &type, nullptr, 4, &result_len) == XF_BUFFER_TOO_SMALL,
          "no buffer at all is refused");
}

// ----------------------------------------------------------------------------------------------------------
// AV and HAL
// ----------------------------------------------------------------------------------------------------------

static void test_av_hal()
{
    printf("\nAV and HAL\n");

    check(AvGetSavedDataAddress() == nullptr, "there is no A/V saved data before the game sets it");
    int marker = 0;
    AvSetSavedDataAddress(&marker);
    check(AvGetSavedDataAddress() == &marker, "and it comes back after it does");
    AvSetSavedDataAddress(nullptr);
    check(AvGetSavedDataAddress() == nullptr, "clearing it works too");

    uint32_t result = 0xFFFFFFFF;
    AvSendTVEncoderOption(nullptr, 6, 0, &result);
    check_u32(result, 0x101, "the A/V capabilities query answers the standard pack");
    AvSendTVEncoderOption(nullptr, 99, 0, &result);
    check_u32(result, 0, "an option the pack does not have answers zero");
    AvSendTVEncoderOption(nullptr, 6, 0, nullptr);
    check(true, "a null result pointer is survivable");

    check_u32(AvSetDisplayMode(nullptr, 0, 0, 0, 0, 0), 0, "AvSetDisplayMode reports success without an A/V pack");

    // The vector arithmetic, including the clamp: a level past the range must not yield IRQL 0, which
    // means "passive level" to whatever reads it.
    uint8_t irql = 0xFF;
    check_u32(HalGetInterruptVector(0, &irql), 0x30, "interrupt level 0 is vector 0x30");
    check_u32(irql, 27, "and implies IRQL 27");
    HalGetInterruptVector(26, &irql);
    check_u32(irql, 1, "the highest normal level implies IRQL 1");
    HalGetInterruptVector(99, &irql);
    check_u32(irql, 1, "a level past the range is clamped to IRQL 1, not 0");
    HalGetInterruptVector(1, nullptr);
    check(true, "a null IRQL pointer is survivable");

    uint8_t regs[256];
    memset(regs, 0xAA, sizeof regs);
    HalReadWritePCISpace(0, 1, 0, regs, 64, 0);
    bool cleared = true;
    for (int i = 0; i < 64; i++)
        if (regs[i]) cleared = false;
    check(cleared, "a configuration-space read reports zeros");
    check_u32(regs[64], 0xAA, "and touches nothing past the length it was given");
}

// ----------------------------------------------------------------------------------------------------------
// interrupts
// ----------------------------------------------------------------------------------------------------------

// Stands in for an ISR: the graphics stack hands one to KeInitializeInterrupt and the loader records it
// without ever calling it (see test_ksync.cpp for the synchronised path).
static uint8_t __attribute__((stdcall)) sync_routine(void *) { return 1; }

static void test_interrupts()
{
    printf("\ninterrupts\n");

    XKInterrupt in;
    memset(&in, 0xCC, sizeof in); // as if the game had not initialised it

    int service_context = 0;
    KeInitializeInterrupt(&in, (void *)sync_routine, &service_context, 0x30 + 11, 11, 2, 0);
    check(in.ServiceRoutine == (void *)sync_routine, "the service routine is recorded");
    check(in.ServiceContext == (void *)&service_context, "the service context is recorded");
    check_u32(in.BusInterruptLevel, 11, "the bus level comes back out of the vector");
    check_u32(in.Irql, 11, "the IRQL is recorded at the width the game reads it with");
    check_u32(in.Connected, 0, "a fresh interrupt is not connected");
    check_u32(in.Mode, 2, "the mode is recorded");
    check_u32(in.ShareVector, 0, "and the share-vector flag");

    check_u32(KeConnectInterrupt(&in), 1, "connecting an interrupt succeeds");
    check_u32(in.Connected, 1, "and it reads back as connected");
    check_u32(KeConnectInterrupt(&in), 0, "connecting it twice does not succeed again");
    check_u32(KeDisconnectInterrupt(&in), 1, "disconnecting reports that it was connected");
    check_u32(in.Connected, 0, "and it reads back as disconnected");
    check_u32(KeDisconnectInterrupt(&in), 0, "disconnecting again reports that it was not");
    // KeSynchronizeExecution belongs to the sync layer and is tested there: it runs the routine under
    // the dispatch mutex, which is xsync's job.
}

// ----------------------------------------------------------------------------------------------------------
// time and FPU
// ----------------------------------------------------------------------------------------------------------

static void test_time()
{
    printf("\ntime\n");

    const int64_t a = kmisc_now_100ns();
    XLargeInteger t;
    KeQuerySystemTime(&t);
    const int64_t b = t.QuadPart;

    // The base matters more than the value: 1601-01-01, the epoch the XDK documents, is 11644473600
    // seconds before 1970. A base of zero or of the Unix epoch would show up as a wildly wrong number.
    const int64_t seconds_since_1601 = b / 10000000;
    const int64_t unix_seconds = seconds_since_1601 - 11644473600LL;
    const int64_t real_unix = (int64_t)time(nullptr);
    const int64_t skew = unix_seconds > real_unix ? unix_seconds - real_unix : real_unix - unix_seconds;
    check(skew <= 5, "KeQuerySystemTime reports the time of day in the console's base (" +
                         std::to_string(skew) + " s from now)");

    const int64_t c = kmisc_now_100ns();
    check(c >= b && c - b < 1000000, "and it moves forward");

    check(KeSaveFloatingPointState(nullptr) == XF_SUCCESS, "KeSaveFloatingPointState succeeds");
    check(KeRestoreFloatingPointState(nullptr) == XF_SUCCESS, "KeRestoreFloatingPointState succeeds");
}

// ----------------------------------------------------------------------------------------------------------
// data exports
// ----------------------------------------------------------------------------------------------------------

static void test_data_exports()
{
    printf("\ndata exports\n");

    // These are read, not called: the game dereferences the thunk slot. The sizes are what the game
    // reads at, so a wrong one is a wrong answer even when every byte is zero.
    check_u32(sizeof(ExEventObjectType), 64, "an object type is 64 bytes");
    check_u32(sizeof(XboxHDKey), 16, "the HD key is 16 bytes");
    check_u32(sizeof(XboxSignatureKey), 16, "the signature key is 16 bytes");
    check_u32(sizeof(XboxAlternateSignatureKeys), 256, "the alternate signature keys are 16 x 16 bytes");
    check_u32(sizeof(IdexChannelObject), 256, "the IDE X channel object is 256 bytes");

    check_u32(XboxKrnlVersion.Major, 1, "the kernel version says major 1");
    check_u32(XboxKrnlVersion.Build, 5838, "and the build the game was compiled against");

    check(XeImageFileName.Length == strlen("\\Device\\CdRom0\\default.xbe"), "the image file name has its length");
    check(XeImageFileName.Buffer != nullptr && strcmp(XeImageFileName.Buffer, "\\Device\\CdRom0\\default.xbe") == 0,
          "and names the image the game was launched from");
    check_u32(XeImageFileName.MaximumLength, XeImageFileName.Length + 1, "with room for the terminator");

    check_u32(HalDiskCachePartitionCount, 3, "the disk cache partition count is 3");
    check_u32(LaunchDataPage == nullptr, 1, "a cold boot has no launch data page");

    // Flags 8 is arcade and flags 2 is a devkit; zero is retail. The revision bytes are the GPU and MCP
    // silicon revisions, which are set on real hardware and are not part of the retail/arcade decision.
    check_u32(XboxHardwareInfo.Flags, 0, "the hardware flags say retail, not arcade (8) or devkit (2)");
    check(XboxHardwareInfo.GpuRevision != 0 && XboxHardwareInfo.McpRevision != 0,
          "the GPU and MCP revisions are reported");

    // Everything that reports zeros must really be zero: the game reads these as "nothing here", and a
    // stray byte would read as a pointer or a length.
    bool type_zeros = true;
    for (size_t i = 0; i < sizeof ExEventObjectType; i++)
        if (ExEventObjectType[i]) type_zeros = false;
    check(type_zeros, "the object-type storage reads as zeros");
}

// ----------------------------------------------------------------------------------------------------------
// XBE sections, against the fixture
// ----------------------------------------------------------------------------------------------------------

static void test_sections()
{
    printf("\nXBE sections\n");

    XbePosix x;
    XbeImage img;
    std::string err;
    if (!x.load_from_memory(bootfix::build(), err)) {
        check(false, "the fixture image loads (" + err + ")");
        return;
    }
    if (!img.map(x, err) || !img.copy_sections(err)) {
        check(false, "the fixture image maps (" + err + ")");
        return;
    }
    kmisc_set_image(&img, &x);

    const XbeSectionInfo *data = x.section_named(".data");
    check(data != nullptr, "the fixture has a .data section");
    if (!data) return;

    // A section header lives in the mapped image; the game passes its address to XeLoadSection.
    XbeSectionHeader *header = (XbeSectionHeader *)(uintptr_t)data->header_va;
    check(header != nullptr && (uint32_t)(uintptr_t)header >= img.base() &&
              (uint32_t)(uintptr_t)header < img.base() + img.span(),
          "the section header is inside the mapped image, where the game will find it");

    uint8_t *body = img.at(data->vaddr);
    check(body != nullptr, "the section body is mapped");
    if (!body) return;

    // The loader counts a preloaded section once, so the fixture's header starts at one; the copy path is
    // the one a count of zero takes, which is what a section the game loads itself gets.
    check_u32(header->reference_count, 1, "a preloaded section arrives already referenced once");
    header->reference_count = 0;
    memset(body, 0xEE, data->raw_size);

    check(XeLoadSection(header) == XF_SUCCESS, "XeLoadSection loads the section the game named");
    check_u32(header->reference_count, 1, "and its reference count goes to one");

    // The bytes must be the section's own: equal to the file's copy, and no longer the corruption.
    const uint8_t *raw = x.data.data() + data->raw_addr;
    check(raw != nullptr && memcmp(body, raw, data->raw_size) == 0,
          "the loaded body is the section's own bytes from the file");
    bool all_ee = true;
    for (uint32_t i = 0; i < data->raw_size; i++)
        if (body[i] != 0xEE) all_ee = false;
    check(!all_ee, "and the corruption is gone");

    // Loading again must not copy a second time: the reference count is what decides, and a section the
    // game has modified in between keeps its modification.
    body[0] ^= 0xFF;

    check(XeLoadSection(header) == XF_SUCCESS, "loading it again succeeds");
    check_u32(header->reference_count, 2, "and counts the second reference");
    check_u32((uint8_t)(body[0] ^ raw[0]), 0xFF, "and does not copy over what the game wrote in between");
    check(XeUnloadSection(header) == XF_SUCCESS, "unloading succeeds");
    check_u32(header->reference_count, 1, "and drops the count to one");
    check(XeUnloadSection(header) == XF_SUCCESS, "unloading again succeeds");
    check_u32(header->reference_count, 0, "and reaches zero");

    // A pointer that is not a section header of this image is refused, not copied from.
    check(XeLoadSection(&err) == XF_INVALID_PARAMETER, "a header this image does not own is refused");
    check(XeLoadSection(nullptr) == XF_INVALID_PARAMETER, "a null header is refused");
    check(XeUnloadSection(nullptr) == XF_INVALID_PARAMETER, "unloading null is refused");

    // With no image wired up the section loader must say so rather than guess.
    kmisc_set_image(nullptr, nullptr);
    check(XeLoadSection(header) == XF_INVALID_PARAMETER, "with no image loaded, loading is refused");
    kmisc_set_image(&img, &x);

    img.unmap();
}

// ----------------------------------------------------------------------------------------------------------
// crypto, against published vectors
// ----------------------------------------------------------------------------------------------------------

static void test_crypto()
{
    printf("\ncrypto\n");

    // FIPS 180-1 test vectors for SHA-1.
    uint8_t ctx[96], digest[20];
    XcSHAInit(ctx);
    XcSHAUpdate(ctx, (const uint8_t *)"abc", 3);
    XcSHAFinal(ctx, digest);
    check(to_hex(digest, 20) == "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA-1 of \"abc\"");

    XcSHAInit(ctx);
    XcSHAFinal(ctx, digest);
    check(to_hex(digest, 20) == "da39a3ee5e6b4b0d3255bfef95601890afd80709", "SHA-1 of the empty string");

    // A million 'a' characters would be slow; a message that spans several blocks is the property that
    // matters, so use 1000 of them: the block boundary handling is what a short vector never reaches.
    XcSHAInit(ctx);
    uint8_t chunk[100];
    memset(chunk, 'a', sizeof chunk);
    for (int i = 0; i < 10; i++) XcSHAUpdate(ctx, chunk, sizeof chunk);
    XcSHAFinal(ctx, digest);
    check(to_hex(digest, 20) == "291e9a6c66994949b57ba5e650361e98fc36b1ba", "SHA-1 of 1000 x \"a\" (multi-block)");

    // RFC 2202 HMAC-SHA-1 test case 1.
    uint8_t key[20];
    memset(key, 0x0b, sizeof key);
    XcHMAC(key, sizeof key, (const uint8_t *)"Hi There", 8, nullptr, 0, digest);
    check(to_hex(digest, 20) == "b617318655057264e28bc0b6fb378c8ef146be00", "HMAC-SHA-1, RFC 2202 case 1");

    // ... and case 2, which hashes data in two pieces.
    const uint8_t key2[] = "Jefe";
    XcHMAC(key2, 4, (const uint8_t *)"what do ya want ", 16, (const uint8_t *)"for nothing?", 12, digest);
    check(to_hex(digest, 20) == "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79", "HMAC-SHA-1, RFC 2202 case 2");

    // Null arguments must not crash: the game is not supposed to pass them, and a stub that crashes on a
    // null it will never see is worse than one that returns zeros.
    XcSHAInit(nullptr);
    XcSHAUpdate(nullptr, nullptr, 0);
    XcSHAFinal(nullptr, nullptr);
    XcHMAC(nullptr, 0, nullptr, 0, nullptr, 0, nullptr);
    check(true, "null crypto arguments are survivable");
}

// ----------------------------------------------------------------------------------------------------------
// the run itself ending on request
// ----------------------------------------------------------------------------------------------------------

// Stands in for a game that never returns, which is the normal case: boot_run_entry has to keep waiting
// until the timeout or until something asks it to stop. Called through the same cdecl, no-argument
// pointer the XBE entry point is called with.
static void spin_forever()
{
    for (;;) sleep(50);
}

static void *request_exit_after(void *arg)
{
    usleep(*(int *)arg * 1000);
    boot_request_exit(4, "KeBugCheck");
    return nullptr;
}

// Read back a log file the logger just wrote.
static std::string read_log(const char *path)
{
    std::string out;
    if (FILE *f = fopen(path, "r")) {
        char line[512];
        while (fgets(line, sizeof line, f)) out += line;
        fclose(f);
    }
    return out;
}

static void test_run_ends_on_request()
{
    printf("\nthe run ending on request\n");

    char logpath[256];
    snprintf(logpath, sizeof logpath, "/tmp/blacklinux-test-%d-runend.log", (int)getpid());
    log_init(logpath, false);
    check(!boot_exit_requested(), "nothing has asked the run to end yet");

    pthread_t thread;
    int delay_ms = 150;
    pthread_create(&thread, nullptr, request_exit_after, &delay_ms);

    const uint32_t stack_commit = 1 << 20;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    uint32_t stack_top = 0;
    const BootOutcome outcome = boot_run_entry((uint32_t)(uintptr_t)&spin_forever, stack_commit, 10000, &stack_top);
    clock_gettime(CLOCK_MONOTONIC, &end);
    pthread_join(thread, nullptr);

    const long elapsed_ms = (end.tv_sec - start.tv_sec) * 1000 + (end.tv_nsec - start.tv_nsec) / 1000000;
    check(outcome == BootOutcome::Exited, "boot_run_entry reports that the run ended on request");
    check(elapsed_ms < 5000, "and it ended when it was asked to, not at the 10 s timeout (" +
                                 std::to_string(elapsed_ms) + " ms)");
    check_u32(stack_top != 0, 1, "and it recorded the boot thread's stack top on the way out");
    check_u32(boot_exit_code(), 4, "the exit code is the one the caller asked for");
    check(std::string(boot_exit_reason()) == "KeBugCheck", "and the reason is the call that asked");

    log_close();
    const std::string log = read_log(logpath);
    check(log.find("exit requested: KeBugCheck (code 4)") != std::string::npos,
          "the request is logged once, with its code");
    check(log.find("boot: the run ended on request (KeBugCheck)") != std::string::npos,
          "and the boot path says which request ended the run");
    unlink(logpath);
}

// ----------------------------------------------------------------------------------------------------------
// the calls that end the run
// ----------------------------------------------------------------------------------------------------------

// Run `child` in a forked process and report what happened: whether the call returned (it must not),
// whether the child was still running when the deadline passed (it should be -- it parked), and what it
// logged.
//
// The log is captured through the logger's own file, not by redirecting stderr: the child's stdio buffers
// do not survive the SIGKILL that ends a parked process, so a stderr capture comes back empty and the
// check would pass or fail for the wrong reason. log_init writes and flushes every line.
struct ChildResult {
    bool returned = false;
    bool still_running = false;
    std::string log;
};

static ChildResult run_child(void (*child)(), const char *tag)
{
    ChildResult r;
    char path[256];
    snprintf(path, sizeof path, "/tmp/blacklinux-test-%d-%s.log", (int)getpid(), tag);
    unlink(path);

    const pid_t pid = fork();
    if (pid == 0) {
        log_init(path, false);
        // The console copies of these lines are the test output's problem, not the assertions'.
        if (!freopen("/dev/null", "w", stderr)) _exit(43);
        child();
        printf("RETURNED\n");
        fflush(stdout);
        log_close();
        _exit(42);
    }

    // Two seconds is far longer than these calls can take to log, and short enough that a suite failure
    // does not stall the run.
    for (int i = 0; i < 200; i++) {
        usleep(10 * 1000);
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            r.returned = true;
            break;
        }
    }
    if (!r.returned) {
        kill(pid, SIGKILL);
        int status = 0;
        waitpid(pid, &status, 0);
        r.still_running = true;
    }
    if (FILE *f = fopen(path, "r")) {
        char line[512];
        while (fgets(line, sizeof line, f)) r.log += line;
        fclose(f);
    }
    unlink(path);
    return r;
}

static void child_bugcheck()
{
    KeBugCheck(0x00000055);
}

static void child_return_to_firmware()
{
    HalReturnToFirmware(1);
}

static void test_fatal_calls()
{
    printf("\ncalls that end the run\n");

    ChildResult bug = run_child(child_bugcheck, "bugcheck");
    check(bug.still_running, "KeBugCheck does not return: the calling thread is parked");
    check(bug.log.find("KeBugCheck(0x00000055)") != std::string::npos,
          "and it logs the bugcheck code");
    // The child inherited the exit request the run-ends test made in this process, so boot_request_exit
    // stays silent for it -- the first request is the one that gets logged, and that is asserted above.
    check(bug.log.find("exit requested:") == std::string::npos,
          "a second exit request is not logged again");

    ChildResult rtf = run_child(child_return_to_firmware, "rtf");
    check(rtf.still_running, "HalReturnToFirmware does not return either");
    check(rtf.log.find("HalReturnToFirmware(1)") != std::string::npos, "and it logs what it was asked for");

    // The request the run ended on, above, is still the one that counts: a second request must not
    // overwrite it, because the first is the reason the run ended and the log already said so.
    check(boot_exit_requested(), "the run's exit request is still recorded");
    check_u32(boot_exit_code(), 4, "with the code the caller asked for");
    check(std::string(boot_exit_reason()) == "KeBugCheck", "and the reason that was given");
    boot_request_exit(0, "second");
    check_u32(boot_exit_code(), 4, "a second request does not overwrite the first");
}

int main()
{
    printf("BLACK loader -- miscellaneous kernel exports\n");

    test_run_ends_on_request(); // first: it sets the process-wide exit request the other tests read
    test_dbgprint_format();
    test_settings();
    test_av_hal();
    test_interrupts();
    test_time();
    test_data_exports();
    test_sections();
    test_crypto();
    test_fatal_calls();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}