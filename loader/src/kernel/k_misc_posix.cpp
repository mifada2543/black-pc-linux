#include "kernel/k_misc_posix.h"

#include "platform/posix/boot.h"
#include "platform/posix/image.h"
#include "platform/posix/log.h"
#include "sha1.h"

#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mutex>

namespace {

void *g_av_saved_data;
char g_image_file_name[] = "\\Device\\CdRom0\\default.xbe";

std::mutex g_section_mutex;
XbeImage *g_image;
const XbePosix *g_xbe;
bool g_widescreen;

} // namespace

// ----------------------------------------------------------------------------------------------------------
// data exports
//
// The game reads these as "the thunk slot's value" and dereferences it. A null slot is a crash on first
// use, which is why every one of the thirteen gets real storage here even when the storage is zeros.

uint8_t ExEventObjectType[64];
uint8_t IoFileObjectType[64];
uint8_t PsThreadObjectType[64];
uint32_t HalDiskCachePartitionCount = 3;
uint32_t HalBootSMCVideoMode = 0;
void *LaunchDataPage = nullptr; // a cold boot has no launch data
XXboxHardwareInfo XboxHardwareInfo = {0, 0xA2, 0xD3, 0, 0}; // retail: not arcade (flag 8), not a devkit (flag 2)
uint8_t XboxHDKey[16];
XXboxKrnlVersion XboxKrnlVersion = {1, 0, 5838, 1};
uint8_t XboxSignatureKey[16];
uint8_t XboxAlternateSignatureKeys[16][16];
XAnsiString XeImageFileName = {(uint16_t)(sizeof g_image_file_name - 1), (uint16_t)sizeof g_image_file_name,
                               g_image_file_name};
uint8_t IdexChannelObject[256];

// ----------------------------------------------------------------------------------------------------------
// AV

extern "C" void *__attribute__((stdcall)) AvGetSavedDataAddress()
{
    // The A/V pack's saved-data pointer: the game stores something here before a quick reboot and reads it
    // back afterwards. There is no A/V pack here, so this is a plain cell the game can use as scratch
    // that survives as long as the process does -- which is the part of the contract it can observe.
    return g_av_saved_data;
}

extern "C" void __attribute__((stdcall)) AvSetSavedDataAddress(void *Address)
{
    logf("misc: AvSetSavedDataAddress(%p)", Address);
    g_av_saved_data = Address;
}

extern "C" void __attribute__((stdcall)) AvSendTVEncoderOption(void *RegisterBase, uint32_t Option, uint32_t Param,
                                                               uint32_t *Result)
{
    if (!Result) return;
    switch (Option) {
    case 6: // AV_QUERY_AV_CAPABILITIES: the standard A/V pack, NTSC-M
        *Result = 0x00000100 | 0x00000001;
        break;
    default:
        *Result = 0;
        break;
    }
    (void)RegisterBase;
    (void)Param;
}

extern "C" uint32_t __attribute__((stdcall)) AvSetDisplayMode(void *RegisterBase, uint32_t Step, uint32_t Mode,
                                                              uint32_t Format, uint32_t Pitch, uint32_t FrameBuffer)
{
    // Display mode setup went to registers on the A/V pack. Nothing here has registers: graphics comes
    // from Vulkan through the port's own device path, so the call only reports that it was made.
    logf("misc: AvSetDisplayMode(%p, step %u, mode %u, format 0x%x, pitch %u, framebuffer 0x%x): no A/V registers "
         "on this port",
         RegisterBase, Step, Mode, Format, Pitch, FrameBuffer);
    return 0;
}

// ----------------------------------------------------------------------------------------------------------
// DbgPrint

size_t kmisc_translate_format(const char *in, char *out, size_t out_size)
{
    if (!out || out_size == 0) return 0;
    if (!in) {
        out[0] = 0;
        return 0;
    }

    size_t n = 0;
    // snprintf semantics: n is what the result would have been, so a caller can see the truncation.
    auto put = [&](char c) {
        if (n + 1 < out_size) out[n] = c;
        n++;
    };

    for (const char *p = in; *p;) {
        if (*p != '%') {
            put(*p++);
            continue;
        }
        put(*p++);
        if (*p == '%') { // "%%": no conversion follows
            put(*p++);
            continue;
        }

        // Find the conversion character: flags, width, precision, then the length modifier.
        const char *q = p;
        while (*q && strchr("-+ #0'", *q)) q++;
        while (*q >= '0' && *q <= '9') q++;
        if (*q == '.') {
            q++;
            while (*q >= '0' && *q <= '9') q++;
        }
        const char *length = q;
        for (;;) {
            if ((q[0] == 'I' && q[1] == '6' && q[2] == '4') || (q[0] == 'I' && q[1] == '3' && q[2] == '2')) q += 3;
            else if (*q == 'h' || *q == 'l' || *q == 'L' || *q == 'z' || *q == 't' || *q == 'j') q++;
            else break;
        }
        const char conv = *q;
        if (conv == 0) { // a truncated directive: copy what is left and stop
            while (*p) put(*p++);
            break;
        }

        switch (conv) {
        case 'p':
            // MSVC prints a pointer as eight uppercase hex digits with no 0x and ignores width flags;
            // glibc prints "0x" and lower case. Games do not parse their own debug output, but a log
            // that reads like the console's is worth the two characters.
            put('0');
            put('8');
            put('X');
            p = q + 1;
            continue;
        case 'S': // MSVC's wide string
            put('l');
            put('s');
            p = q + 1;
            continue;
        case 'C': // MSVC's wide character
            put('l');
            put('c');
            p = q + 1;
            continue;
        default:
            break;
        }

        while (p < length) put(*p++); // flags, width, precision unchanged
        if (length[0] == 'I') {
            // "I64" is MSVC's ll and "I32" needs no modifier at all; either way the "I" and the digits go.
            if (length[1] == '6') put('l'), put('l');
            p = q;
        } else {
            while (p < q) put(*p++);
        }
        put(conv);
        p = q + 1;
    }

    out[n < out_size ? n : out_size - 1] = 0;
    return n;
}

extern "C" uint32_t __attribute__((cdecl)) DbgPrint(const char *Format, ...)
{
    // The format is translated before it reaches libc; a game that prints a 64-bit value with "%I64x"
    // would otherwise get the literal text and a misread argument. Formats are compile-time literals in
    // the game, so 512 bytes is not a limit anything real will reach.
    char fmt[512];
    kmisc_translate_format(Format, fmt, sizeof fmt);

    char msg[1024];
    va_list ap;
    va_start(ap, Format);
    const int written = vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (written < 0) return 0;

    size_t len = strlen(msg);
    while (len && (msg[len - 1] == '\n' || msg[len - 1] == '\r')) msg[--len] = 0;
    logf("[DbgPrint] %s", msg);
    return (uint32_t)len; // what an Xbox DbgPrint returns
}

// ----------------------------------------------------------------------------------------------------------
// non-volatile settings

extern "C" NTSTATUS __attribute__((stdcall)) ExQueryNonVolatileSetting(uint32_t ValueIndex, uint32_t *Type, void *Value,
                                                                       uint32_t ValueLength, uint32_t *ResultLength)
{
    uint32_t dword = 0, size = 4, type = XREG_DWORD;
    bool known = true;
    switch (ValueIndex) {
    case 0x000: dword = 0; break;                  // XC_TIMEZONE_BIAS (UTC)
    case 0x001: case 0x004: type = XREG_BINARY; break; // XC_TZ_STD_NAME / XC_TZ_DLT_NAME
    case 0x002: case 0x005: type = XREG_BINARY; break; // XC_TZ_STD_DATE / XC_TZ_DLT_DATE
    case 0x003: case 0x006: dword = 0; break;      // XC_TZ_STD_BIAS / XC_TZ_DLT_BIAS
    case 0x007: dword = 1; break;                  // XC_LANGUAGE: English
    case 0x008: dword = g_widescreen ? 0x00010000 : 0; break; // XC_VIDEO: bit 16 = widescreen TV
    case 0x009: dword = 0; break;                  // XC_AUDIO: stereo
    case 0x00A: dword = 0; break;                  // XC_P_CONTROL_GAMES: no limit
    case 0x00B: dword = 0; break;                  // XC_P_CONTROL_PASSWORD
    case 0x00C: dword = 0; break;                  // XC_P_CONTROL_MOVIES
    case 0x011: dword = 0; break;                  // XC_MISC
    case 0x012: dword = 1; break;                  // XC_DVD_REGION
    case 0x103: dword = 0x00400100; break;         // XC_FACTORY_AV_REGION: NTSC-M, 60 Hz
    case 0x104: dword = 1; break;                  // XC_FACTORY_GAME_REGION: North America
    case 0x0FF: case 0xFFFF: // whole sections: zeros, truncated to what the caller asked for
        type = XREG_BINARY;
        size = ValueLength < 256 ? ValueLength : 256;
        break;
    default: known = false; break;
    }

    if (!known) {
        logf("misc: ExQueryNonVolatileSetting(0x%x): no such setting", ValueIndex);
        return XF_OBJECT_NAME_NOT_FOUND;
    }
    if (!Value || ValueLength < size) return XF_BUFFER_TOO_SMALL;
    if (Type) *Type = type;
    memset(Value, 0, size);
    if (type == XREG_DWORD) memcpy(Value, &dword, 4);
    if (ResultLength) *ResultLength = size;
    return XF_SUCCESS;
}

void kmisc_set_widescreen(bool on) { g_widescreen = on; }
bool kmisc_widescreen() { return g_widescreen; }

// ----------------------------------------------------------------------------------------------------------
// HAL

extern "C" uint32_t __attribute__((stdcall)) HalGetInterruptVector(uint32_t BusInterruptLevel, uint8_t *Irql)
{
    // The console's PCI interrupt vectors start at 0x30 and the IRQL a vector implies counts down from
    // DISPATCH_LEVEL. The clamp at 1 is the reference's: a level past the range must not produce a zero
    // IRQL, which means "at passive level" to the code that reads it.
    if (Irql) *Irql = (uint8_t)(BusInterruptLevel < 27 ? 27 - BusInterruptLevel : 1);
    return BusInterruptLevel + 0x30;
}

extern "C" void __attribute__((stdcall)) HalReadWritePCISpace(uint32_t BusNumber, uint32_t SlotNumber,
                                                              uint32_t RegisterNumber, void *Buffer, uint32_t Length,
                                                              uint8_t WritePCISpace)
{
    // No PCI bus here. A read reports zeros, which is what an empty configuration space reads as; a
    // write is dropped, because there is nothing behind it to change.
    if (!WritePCISpace && Buffer) memset(Buffer, 0, Length);
    (void)BusNumber;
    (void)SlotNumber;
    (void)RegisterNumber;
}

extern "C" void __attribute__((stdcall)) HalRegisterShutdownNotification(void *ShutdownRegistration, uint8_t Register)
{
    logf("misc: HalRegisterShutdownNotification(%p, register %u): recorded, nothing to shut down",
         ShutdownRegistration, Register);
}

extern "C" void __attribute__((stdcall)) HalReturnToFirmware(uint32_t Routine)
{
    logc("misc: HalReturnToFirmware(%u): the game asked to leave (0 halt, 1 reboot, 2 quick reboot, 3 fatal error)",
         Routine);
    if (LaunchDataPage) {
        const uint32_t *ld = (const uint32_t *)LaunchDataPage;
        logf("    launch data page %p: type %u, title %08X, flags 0x%x", LaunchDataPage, ld[0], ld[1], ld[2]);
    }
    boot_request_exit(Routine == 3 ? 1 : 0, "HalReturnToFirmware");
    // Do not return: the console would be gone, and whatever the game does after asking to leave is not
    // something to keep running. boot_run_entry sees the request and ends the run.
    for (;;) sleep(3600);
}

extern "C" void __attribute__((stdcall)) HalInitiateShutdown()
{
    logc("misc: HalInitiateShutdown(): the game asked to shut the console down");
    boot_request_exit(0, "HalInitiateShutdown");
    for (;;) sleep(3600);
}

extern "C" void __attribute__((fastcall)) IofCompleteRequest(void *Irp, char PriorityBoost)
{
    logf("misc: IofCompleteRequest(%p, boost %d): no I/O stack on this port", Irp, PriorityBoost);
}

extern "C" void __attribute__((stdcall)) KeBugCheck(uint32_t BugCheckCode)
{
    logc("misc: KeBugCheck(0x%08x): the game bugchecked", BugCheckCode);
    boot_request_exit(4, "KeBugCheck");
    for (;;) sleep(3600);
}

// ----------------------------------------------------------------------------------------------------------
// interrupts

extern "C" void __attribute__((stdcall)) KeInitializeInterrupt(XKInterrupt *Interrupt, void *ServiceRoutine,
                                                               void *ServiceContext, uint32_t Vector, uint8_t Irql,
                                                               uint32_t InterruptMode, uint8_t ShareVector)
{
    if (!Interrupt) return;
    Interrupt->ServiceRoutine = ServiceRoutine;
    Interrupt->ServiceContext = ServiceContext;
    Interrupt->BusInterruptLevel = Vector - 0x30;
    Interrupt->Irql = Irql;
    Interrupt->Mode = (uint8_t)InterruptMode;
    Interrupt->ShareVector = ShareVector;
    Interrupt->Connected = 0;
    Interrupt->ServiceCount = 0;
}

extern "C" uint8_t __attribute__((stdcall)) KeConnectInterrupt(XKInterrupt *Interrupt)
{
    if (!Interrupt) return 0;
    if (Interrupt->Connected) return 0; // an interrupt that is already connected stays connected
    logf("misc: KeConnectInterrupt(%p, level %u): recorded, will never fire", Interrupt, Interrupt->BusInterruptLevel);
    Interrupt->Connected = 1;
    return 1;
}

extern "C" uint8_t __attribute__((stdcall)) KeDisconnectInterrupt(XKInterrupt *Interrupt)
{
    if (!Interrupt) return 0;
    const bool was = Interrupt->Connected != 0;
    Interrupt->Connected = 0;
    return was ? 1 : 0;
}

// ----------------------------------------------------------------------------------------------------------
// time, FPU

int64_t kmisc_now_100ns()
{
    // 100-nanosecond units since 1601-01-01, which is the base the XDK documents for KeQuerySystemTime
    // and the base the reference's GetSystemTimePreciseAsFileTime produces. 11644473600 seconds separate
    // that epoch from 1970.
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 10000000LL + ts.tv_nsec / 100 + 116444736000000000LL;
}

extern "C" void __attribute__((stdcall)) KeQuerySystemTime(XLargeInteger *CurrentTime)
{
    if (CurrentTime) CurrentTime->QuadPart = kmisc_now_100ns();
}

extern "C" NTSTATUS __attribute__((stdcall)) KeSaveFloatingPointState(void *FloatSave)
{
    // Nothing to save: on Linux every thread has its own FPU state and the game's threads are its own.
    (void)FloatSave;
    return XF_SUCCESS;
}

extern "C" NTSTATUS __attribute__((stdcall)) KeRestoreFloatingPointState(void *FloatSave)
{
    (void)FloatSave;
    return XF_SUCCESS;
}

// ----------------------------------------------------------------------------------------------------------
// XBE sections

void kmisc_set_image(XbeImage *img, const XbePosix *x)
{
    std::lock_guard<std::mutex> lock(g_section_mutex);
    g_image = img;
    g_xbe = x;
}

extern "C" NTSTATUS __attribute__((stdcall)) XeLoadSection(void *Section)
{
    std::lock_guard<std::mutex> lock(g_section_mutex);
    if (!g_xbe || !g_image) {
        logf("misc: XeLoadSection(%p): no image is loaded", Section);
        return XF_INVALID_PARAMETER;
    }

    const XbeSectionInfo *found = nullptr;
    for (const auto &s : g_xbe->sections)
        if (s.header_va == (uint32_t)(uintptr_t)Section) found = &s;
    if (!found) {
        logf("misc: XeLoadSection(%p): not a section header of this image", Section);
        return XF_INVALID_PARAMETER;
    }

    // The reference count lives in the mapped image, where the game can read it too.
    XbeSectionHeader *header = (XbeSectionHeader *)Section;
    if (header->reference_count++ == 0) {
        uint8_t *dst = g_image->at(found->vaddr);
        if (!dst || found->raw_size > found->vsize) {
            header->reference_count--;
            logf("misc: XeLoadSection('%s' at 0x%08x): not writable or raw_size > vsize", found->name.c_str(),
                 found->vaddr);
            return XF_INVALID_PARAMETER;
        }
        memcpy(dst, g_xbe->data.data() + found->raw_addr, found->raw_size);
        if (found->vsize > found->raw_size) memset(dst + found->raw_size, 0, found->vsize - found->raw_size);
    }
    logf("misc: XeLoadSection('%s' at 0x%08x, 0x%x bytes) -> refcount %u", found->name.c_str(), found->vaddr,
         found->vsize, header->reference_count);
    return XF_SUCCESS;
}

extern "C" NTSTATUS __attribute__((stdcall)) XeUnloadSection(void *Section)
{
    std::lock_guard<std::mutex> lock(g_section_mutex);
    if (!Section) return XF_INVALID_PARAMETER;
    XbeSectionHeader *header = (XbeSectionHeader *)Section;
    if (header->reference_count) header->reference_count--;
    // The pages stay: the whole image is one mapping, so there is nothing to give back, and the game has
    // no way to tell the difference unless it reads the section after unloading it.
    logf("misc: XeUnloadSection(%p) -> refcount %u", Section, header->reference_count);
    return XF_SUCCESS;
}

// ----------------------------------------------------------------------------------------------------------
// crypto
//
// sha1.cpp is the loader's own, unchanged: it is plain C++ with no Windows dependency, and it is the same
// code the Xbox path used, so a digest cannot differ between the two ports.

extern "C" void __attribute__((stdcall)) XcSHAInit(uint8_t *Context)
{
    if (Context) sha1_init((Sha1Ctx *)Context);
}

extern "C" void __attribute__((stdcall)) XcSHAUpdate(uint8_t *Context, const uint8_t *Input, uint32_t InputLength)
{
    if (Context && Input) sha1_update((Sha1Ctx *)Context, Input, InputLength);
}

extern "C" void __attribute__((stdcall)) XcSHAFinal(uint8_t *Context, uint8_t *Digest)
{
    if (Context && Digest) sha1_final((Sha1Ctx *)Context, Digest);
}

extern "C" void __attribute__((stdcall)) XcHMAC(const uint8_t *Key, uint32_t KeyLength, const uint8_t *Data,
                                               uint32_t DataLength, const uint8_t *Data2, uint32_t Data2Length,
                                               uint8_t *Digest)
{
    if (Digest) hmac_sha1(Key, KeyLength, Data, DataLength, Data2, Data2Length, Digest);
}