// The Rtl* exports: string conversions, time conversions, critical sections, and the status-to-Win32
// mapping. The POSIX half of loader/src/kernel/k_rtl.cpp, which stays the reference.
//
// Three of these need more than a transliteration, and the reasons are worth stating:
//
//   * RtlNtStatusToDosError has no host to delegate to. The table below covers every status the file,
//     sync and memory layers can return, because those are the ones the game will see; anything else
//     becomes ERROR_GEN_FAILURE, which is what the real function does for a status it does not know.
//   * RtlTimeToTimeFields / RtlTimeFieldsToTime are the Xbox's only way to read the clock in parts, so
//     they carry the conversion the Windows port got from ntdll. The arithmetic is done here (days from
//     a civil date and back) rather than through time_t, because the base is 1601 and the field struct
//     is the Xbox's, not the host's. tests/test_krtl.cpp checks every conversion against gmtime_r, which
//     is an independent implementation of the same thing.
//   * The critical-section calls are Rtl* by name but synchronisation by nature, so they land on xsync's
//     28-byte-layout implementation rather than growing a second one.
#pragma once

#include <stdint.h>

#include "platform/posix/xfile.h"  // XAnsiString, the other string shape
#include "platform/posix/xsync.h"  // XRtlCriticalSection, XK_SUCCESS

// XUNICODE_STRING: the wide counterpart of XAnsiString, same 8-byte shape.
struct XUnicodeString {
    uint16_t Length;
    uint16_t MaximumLength;
    wchar_t *Buffer;
};
static_assert(sizeof(XUnicodeString) == 8, "UNICODE_STRING is two USHORTs and a pointer");

// TIME_FIELDS, as RtlTimeToTimeFields fills it: eight USHORTs, no padding surprises.
struct XTimeFields {
    uint16_t Year;
    uint16_t Month;
    uint16_t Day;
    uint16_t Weekday;
    uint16_t Hour;
    uint16_t Minute;
    uint16_t Second;
    uint16_t Milliseconds;
};
static_assert(sizeof(XTimeFields) == 16, "TIME_FIELDS is eight USHORTs");

#define KR_SUCCESS ((NTSTATUS)0x00000000)
#define KR_BUFFER_OVERFLOW ((NTSTATUS)0x80000005)
#define KR_UNSUCCESSFUL ((NTSTATUS)0xC0000001)
#define KR_NO_MEMORY ((NTSTATUS)0xC0000017)

extern "C" NTSTATUS __attribute__((stdcall)) RtlAnsiStringToUnicodeString(XUnicodeString *Destination,
                                                                         const XAnsiString *Source,
                                                                         uint8_t Allocate);
extern "C" NTSTATUS __attribute__((stdcall)) RtlUnicodeStringToAnsiString(XAnsiString *Destination,
                                                                         const XUnicodeString *Source,
                                                                         uint8_t Allocate);
extern "C" uint32_t __attribute__((stdcall)) RtlCompareMemoryUlong(const void *Source, uint32_t Length,
                                                                    uint32_t Pattern);
extern "C" uint8_t __attribute__((stdcall)) RtlEqualString(const XAnsiString *String1,
                                                            const XAnsiString *String2,
                                                            uint8_t CaseInSensitive);
extern "C" void __attribute__((stdcall)) RtlInitAnsiString(XAnsiString *Destination, const char *Source);
extern "C" uint32_t __attribute__((stdcall)) RtlNtStatusToDosError(NTSTATUS Status);
extern "C" uint8_t __attribute__((stdcall)) RtlTimeFieldsToTime(const XTimeFields *TimeFields,
                                                                 struct XLargeInteger *Time);
extern "C" void __attribute__((stdcall)) RtlTimeToTimeFields(const struct XLargeInteger *Time,
                                                             XTimeFields *TimeFields);

extern "C" void __attribute__((stdcall)) RtlInitializeCriticalSection(XRtlCriticalSection *CriticalSection);
extern "C" void __attribute__((stdcall)) RtlEnterCriticalSection(XRtlCriticalSection *CriticalSection);
extern "C" void __attribute__((stdcall)) RtlLeaveCriticalSection(XRtlCriticalSection *CriticalSection);

// Exposed for the tests: the civil-date arithmetic the two time calls share, in the Xbox's own frame --
// day 0 is 1601-01-01, which is what both the era formulas and the weekday count are shifted by.
int64_t kr_days_from_civil(int32_t y, uint32_t m, uint32_t d);
void kr_civil_from_days(int64_t z, int32_t *y, uint32_t *m, uint32_t *d);
uint8_t kr_weekday_from_days(int64_t z); // 0 = Sunday
// Days from 1601-01-01 to 1970-01-01, and the 100 ns units in a second: the two constants every
// conversion between the Xbox's clock and a host calendar goes through.
constexpr int64_t KR_1601_EPOCH_OFFSET = 134774;
constexpr int64_t KR_UNITS_PER_SECOND = 10000000;
constexpr int64_t KR_UNIX_EPOCH_OFFSET = 116444736000000000LL; // 1601-01-01 in 100 ns units