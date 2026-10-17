// Hardware-facing and miscellaneous kernel exports: AV, HAL, interrupts, non-volatile settings,
// XBE section loading, crypto, and the data exports. The POSIX half of loader/src/kernel/k_misc.cpp,
// which stays the reference for the Xbox-visible behaviour.
//
// What carries over unchanged, because the game reads it:
//
//   * the data exports' values. XeImageFileName, XboxKrnlVersion and XboxHardwareInfo are not calls:
//     the game reads the thunk slot and dereferences it, so a slot that is still null is a crash on
//     first use, not a missing feature. This module gives all thirteen real storage.
//   * the non-volatile setting table, index for index: language, video, audio, region, DVD region.
//     XAPI reads these during startup and the game branches on them.
//   * HalGetInterruptVector's arithmetic, including the IRQL clamp, because the graphics stack asks
//     for vectors and checks the level it gets back.
//   * KINTERRUPT's field layout, because the game allocates the struct and reads back what we write.
//   * KeQuerySystemTime's base: 100-nanosecond units since 1601-01-01, the NT epoch the XDK documents,
//     so a game that converts to seconds does not need a 369-year offset the console never had.
//
// Two things need more than a copy, and both are why this file is not a transliteration:
//
//   * DbgPrint takes an MSVC format string. glibc does not understand MSVC's size prefixes, so "%I64x"
//     would print the literal text and misread the argument after it. The format is translated first
//     (kmisc_translate_format), which is the only place in the port where the *output* of the game's
//     own logging depends on host libc behaviour.
//   * HalReturnToFirmware, HalInitiateShutdown and KeBugCheck have to stop the game. On the console
//     they end the session; here they set the loader's exit request (boot.h) and park the calling
//     thread, and boot_run_entry notices and returns, so a game that bugs out exits cleanly with its
//     reason in the log instead of hanging until the timeout.
#pragma once

#include <stdint.h>
#include <stdio.h>

#include "platform/posix/xfile.h" // XAnsiString, XLargeInteger, XF_* status codes

// ---- Xbox kernel ABI pieces this module needs, defined locally (as k_memory_posix.h and xfile.h do) --

struct XKInterrupt {
    void *ServiceRoutine;
    void *ServiceContext;
    uint32_t BusInterruptLevel;
    uint32_t Irql;
    uint8_t Connected;
    uint8_t ShareVector;
    uint8_t Mode;
    uint8_t Padding;
    uint32_t ServiceCount;
    uint32_t DispatchCode[22];
};
static_assert(sizeof(XKInterrupt) == 112, "KINTERRUPT as the Xbox lays it out");

struct XXboxHardwareInfo {
    uint32_t Flags;
    uint8_t GpuRevision;
    uint8_t McpRevision;
    uint8_t Unknown3;
    uint8_t Unknown4;
};
static_assert(sizeof(XXboxHardwareInfo) == 8, "XBOX_HARDWARE_INFO is 8 bytes");

struct XXboxKrnlVersion {
    uint16_t Major, Minor, Build, Qfe;
};
static_assert(sizeof(XXboxKrnlVersion) == 8, "XBOX_KRNL_VERSION is four USHORTs");

enum : uint32_t { XREG_BINARY = 3, XREG_DWORD = 4 };

// ---- data exports -----------------------------------------------------------------------------------------
//
// Every one of these is read by the game as "the thunk slot's value", so each must be a real address
// with the size the Xbox gives it. The three object types are 64 bytes of type-object storage that only
// ObReferenceObjectByHandle-style code would recognise; they stay zero, but they must exist.

extern uint8_t ExEventObjectType[64];
extern uint8_t IoFileObjectType[64];
extern uint8_t PsThreadObjectType[64];
extern uint32_t HalDiskCachePartitionCount;
extern uint32_t HalBootSMCVideoMode;
extern void *LaunchDataPage; // a cold boot has no launch data
extern XXboxHardwareInfo XboxHardwareInfo;
extern uint8_t XboxHDKey[16];
extern XXboxKrnlVersion XboxKrnlVersion;
extern uint8_t XboxSignatureKey[16];
extern uint8_t XboxAlternateSignatureKeys[16][16];
extern XAnsiString XeImageFileName;
extern uint8_t IdexChannelObject[256];

// ---- AV ---------------------------------------------------------------------------------------------------

extern "C" void *__attribute__((stdcall)) AvGetSavedDataAddress();
extern "C" void __attribute__((stdcall)) AvSetSavedDataAddress(void *Address);
extern "C" void __attribute__((stdcall)) AvSendTVEncoderOption(void *RegisterBase, uint32_t Option, uint32_t Param,
                                                               uint32_t *Result);
extern "C" uint32_t __attribute__((stdcall)) AvSetDisplayMode(void *RegisterBase, uint32_t Step, uint32_t Mode,
                                                              uint32_t Format, uint32_t Pitch, uint32_t FrameBuffer);

// The one cdecl export here, and the reason kmisc_translate_format exists. Returns the number of
// characters printed, which is what an Xbox DbgPrint returns and what a game that stores the result
// expects.
extern "C" uint32_t __attribute__((cdecl)) DbgPrint(const char *Format, ...);

// ---- non-volatile settings -------------------------------------------------------------------------------

extern "C" NTSTATUS __attribute__((stdcall)) ExQueryNonVolatileSetting(uint32_t ValueIndex, uint32_t *Type, void *Value,
                                                                       uint32_t ValueLength, uint32_t *ResultLength);

// ---- HAL -------------------------------------------------------------------------------------------------

extern "C" uint32_t __attribute__((stdcall)) HalGetInterruptVector(uint32_t BusInterruptLevel, uint8_t *Irql);
extern "C" void __attribute__((stdcall)) HalReadWritePCISpace(uint32_t BusNumber, uint32_t SlotNumber,
                                                              uint32_t RegisterNumber, void *Buffer, uint32_t Length,
                                                              uint8_t WritePCISpace);
extern "C" void __attribute__((stdcall)) HalRegisterShutdownNotification(void *ShutdownRegistration, uint8_t Register);
extern "C" void __attribute__((stdcall)) HalReturnToFirmware(uint32_t Routine);
extern "C" void __attribute__((stdcall)) HalInitiateShutdown();
extern "C" void __attribute__((fastcall)) IofCompleteRequest(void *Irp, char PriorityBoost);
extern "C" void __attribute__((stdcall)) KeBugCheck(uint32_t BugCheckCode);

// ---- interrupts ------------------------------------------------------------------------------------------
//
// Recorded, never delivered: the only interrupts on this port would be the GPU's and the audio DSP's,
// and both belong to the libraries being replaced. Keeping the objects honest matters anyway -- the
// graphics stack allocates KINTERRUPTs and reads Connected back.

extern "C" void __attribute__((stdcall)) KeInitializeInterrupt(XKInterrupt *Interrupt, void *ServiceRoutine,
                                                               void *ServiceContext, uint32_t Vector, uint8_t Irql,
                                                               uint32_t InterruptMode, uint8_t ShareVector);
extern "C" uint8_t __attribute__((stdcall)) KeConnectInterrupt(XKInterrupt *Interrupt);
extern "C" uint8_t __attribute__((stdcall)) KeDisconnectInterrupt(XKInterrupt *Interrupt);
// KeSynchronizeExecution is not here: it synchronises through the dispatch mutex, which belongs to the
// sync layer (k_sync_posix.h).

// ---- time, FPU -------------------------------------------------------------------------------------------

extern "C" void __attribute__((stdcall)) KeQuerySystemTime(XLargeInteger *CurrentTime);
extern "C" NTSTATUS __attribute__((stdcall)) KeSaveFloatingPointState(void *FloatSave);
extern "C" NTSTATUS __attribute__((stdcall)) KeRestoreFloatingPointState(void *FloatSave);

// ---- XBE sections ----------------------------------------------------------------------------------------

extern "C" NTSTATUS __attribute__((stdcall)) XeLoadSection(void *Section);
extern "C" NTSTATUS __attribute__((stdcall)) XeUnloadSection(void *Section);

// ---- crypto ----------------------------------------------------------------------------------------------

extern "C" void __attribute__((stdcall)) XcSHAInit(uint8_t *Context);
extern "C" void __attribute__((stdcall)) XcSHAUpdate(uint8_t *Context, const uint8_t *Input, uint32_t InputLength);
extern "C" void __attribute__((stdcall)) XcSHAFinal(uint8_t *Context, uint8_t *Digest);
extern "C" void __attribute__((stdcall)) XcHMAC(const uint8_t *Key, uint32_t KeyLength, const uint8_t *Data,
                                               uint32_t DataLength, const uint8_t *Data2, uint32_t Data2Length,
                                               uint8_t *Digest);

// ---- module plumbing -------------------------------------------------------------------------------------

// Tell the section loader which image is running, so XeLoadSection can find a section header the game
// hands it. boot_prepare does this; without it XeLoadSection refuses rather than guessing.
void kmisc_set_image(struct XbeImage *img, const struct XbePosix *x);

// Whether XC_VIDEO reports a widescreen TV. Off by default, matching the reference's 4:3 default.
void kmisc_set_widescreen(bool on);
bool kmisc_widescreen();

// Rewrite an MSVC format string into one glibc's printf understands. Returns the length written, the
// way snprintf does, and truncates if `out_size` is too small. Exposed because it is the only piece of
// this module with behaviour worth testing directly: a wrong translation prints garbage without
// crashing, which is exactly the kind of bug that would otherwise reach the game unnoticed.
size_t kmisc_translate_format(const char *in, char *out, size_t out_size);

// 100-nanosecond units since 1601-01-01, the base KeQuerySystemTime reports.
int64_t kmisc_now_100ns();