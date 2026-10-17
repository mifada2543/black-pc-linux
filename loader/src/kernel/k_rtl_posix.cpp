#include "kernel/k_rtl_posix.h"

#include "kernel/k_memory_posix.h" // ExAllocatePoolWithTag, for the Allocate=true case
#include "platform/posix/log.h"

#include <ctype.h>
#include <string.h>

// The pool tag the reference uses for these buffers: 'grtS' is "Strg" spelled the way a tag is, which
// is how an MSVC build of the same code would have written it.
constexpr uint32_t kStrgTag = 'g' | ('r' << 8) | ('t' << 16) | ('S' << 24);

// ----------------------------------------------------------------------------------------------------------
// civil dates
//
// Days-to-civil and civil-to-days, from the standard era-based algorithms: proleptic Gregorian, shifted
// so that day 0 is 1601-01-01. Nothing here reads the host clock, so the conversion is the same on every
// machine and can be checked against gmtime_r without the two disagreeing about time zones.

int64_t kr_days_from_civil(int32_t y, uint32_t m, uint32_t d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = (uint32_t)(y - era * 400);              // [0, 399]
    const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
    const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;  // [0, 146096]
    // The era arithmetic gives days since 1970-01-01; the constant moves that to the Xbox's own frame,
    // 1601-01-01, so day 0 is day 0 everywhere in this file.
    return era * 146097 + (int64_t)doe - 719468 + KR_1601_EPOCH_OFFSET;
}

void kr_civil_from_days(int64_t z, int32_t *y, uint32_t *m, uint32_t *d)
{
    z += 719468 - KR_1601_EPOCH_OFFSET; // back to the Unix frame the era arithmetic works in
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = (uint32_t)(z - era * 146097);          // [0, 146096]
    const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; // [0, 399]
    const int32_t yr = (int32_t)yoe + (int32_t)era * 400;
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0, 365]
    const uint32_t mp = (5 * doy + 2) / 153;                      // [0, 11]
    const uint32_t day = doy - (153 * mp + 2) / 5 + 1;            // [1, 31]
    const uint32_t month = mp + (mp < 10 ? 3 : -9);               // [1, 12]
    if (y) *y = yr + (month <= 2);
    if (m) *m = month;
    if (d) *d = day;
}

// Weekday of a day count, with Sunday = 0 -- the numbering TIME_FIELDS uses. Day 0 is 1601-01-01, which
// was a Monday, so the offset is one rather than zero. (Checked two ways in tests/test_krtl.cpp: against
// gmtime_r for modern dates, and against Zeller's congruence for the ones a 32-bit time_t cannot hold.)
// An earlier version of this said Saturday, which is off by one day and would have made every weekday the
// game prints wrong.
uint8_t kr_weekday_from_days(int64_t z)
{
    int64_t wd = (z + 1) % 7;
    if (wd < 0) wd += 7;
    return (uint8_t)wd;
}

// ----------------------------------------------------------------------------------------------------------
// strings

extern "C" NTSTATUS __attribute__((stdcall)) RtlAnsiStringToUnicodeString(XUnicodeString *Destination,
                                                                         const XAnsiString *Source,
                                                                         uint8_t Allocate)
{
    if (!Destination || !Source || !Source->Buffer) return KR_UNSUCCESSFUL;
    const uint32_t n = Source->Length;
    if (Allocate) {
        Destination->Buffer = (wchar_t *)ExAllocatePoolWithTag((n + 1) * 2, kStrgTag);
        if (!Destination->Buffer) return KR_NO_MEMORY;
        Destination->MaximumLength = (uint16_t)((n + 1) * 2);
    } else if (Destination->MaximumLength < n * 2) {
        return KR_BUFFER_OVERFLOW;
    }
    for (uint32_t i = 0; i < n; i++) Destination->Buffer[i] = (wchar_t)(unsigned char)Source->Buffer[i];
    Destination->Length = (uint16_t)(n * 2);
    if (Destination->MaximumLength >= (n + 1) * 2) Destination->Buffer[n] = 0;
    return KR_SUCCESS;
}

extern "C" NTSTATUS __attribute__((stdcall)) RtlUnicodeStringToAnsiString(XAnsiString *Destination,
                                                                         const XUnicodeString *Source,
                                                                         uint8_t Allocate)
{
    if (!Destination || !Source || !Source->Buffer) return KR_UNSUCCESSFUL;
    const uint32_t n = Source->Length / 2;
    if (Allocate) {
        Destination->Buffer = (char *)ExAllocatePoolWithTag(n + 1, kStrgTag);
        if (!Destination->Buffer) return KR_NO_MEMORY;
        Destination->MaximumLength = (uint16_t)(n + 1);
    } else if (Destination->MaximumLength < n) {
        return KR_BUFFER_OVERFLOW;
    }
    for (uint32_t i = 0; i < n; i++) {
        const wchar_t c = Source->Buffer[i];
        // A character the console's code page cannot show becomes '?', which is what narrowing a wide
        // string to bytes does everywhere else; a game comparing paths would see the difference, but a
        // game printing one would rather see this than a truncated string.
        Destination->Buffer[i] = c < 256 ? (char)c : '?';
    }
    Destination->Length = (uint16_t)n;
    if (Destination->MaximumLength > n) Destination->Buffer[n] = 0;
    return KR_SUCCESS;
}

extern "C" uint32_t __attribute__((stdcall)) RtlCompareMemoryUlong(const void *Source, uint32_t Length,
                                                                    uint32_t Pattern)
{
    // The Xbox's version compares ULONGs and returns the offset of the first that differs, so the source
    // has to be ULONG-aligned -- which is why Length is a byte count and the answer a multiple of 4.
    const uint32_t *p = (const uint32_t *)Source;
    uint32_t i = 0;
    for (; i < Length / 4; i++)
        if (p[i] != Pattern) break;
    return i * 4;
}

extern "C" uint8_t __attribute__((stdcall)) RtlEqualString(const XAnsiString *String1,
                                                            const XAnsiString *String2,
                                                            uint8_t CaseInSensitive)
{
    if (!String1 || !String2) return 0;
    if (String1->Length != String2->Length) return 0;
    for (uint32_t i = 0; i < String1->Length; i++) {
        char a = String1->Buffer ? String1->Buffer[i] : 0;
        char b = String2->Buffer ? String2->Buffer[i] : 0;
        if (CaseInSensitive) {
            a = (char)toupper((unsigned char)a);
            b = (char)toupper((unsigned char)b);
        }
        if (a != b) return 0;
    }
    return 1;
}

extern "C" void __attribute__((stdcall)) RtlInitAnsiString(XAnsiString *Destination, const char *Source)
{
    if (!Destination) return;
    Destination->Buffer = (char *)Source;
    if (Source) {
        const size_t n = strlen(Source);
        Destination->Length = (uint16_t)n;
        Destination->MaximumLength = (uint16_t)(n + 1);
    } else {
        Destination->Length = 0;
        Destination->MaximumLength = 0;
    }
}

// ----------------------------------------------------------------------------------------------------------
// status to Win32 error
//
// The game's CRT and XAPI branch on these: a "file not found" has to arrive as ERROR_FILE_NOT_FOUND, not
// as an NT status, or the retry logic never runs. Every status the port's own layers return is here.

extern "C" uint32_t __attribute__((stdcall)) RtlNtStatusToDosError(NTSTATUS Status)
{
    switch ((uint32_t)Status) {
    case 0x00000000u: return 0;         // ERROR_SUCCESS
    case 0x00000102u: return 258;        // WAIT_TIMEOUT
    case 0x00000103u: return 997;        // ERROR_IO_PENDING
    case 0x00000080u: return 735;        // ERROR_ABANDONED_WAIT_0
    case 0x80000004u: return 735;        // ERROR_ABANDONED_WAIT_0 (inherited flavour)
    case 0x80000005u: return 234;        // ERROR_MORE_DATA
    case 0x80000006u: return 18;         // ERROR_NO_MORE_FILES
    case 0xC0000001u: return 31;         // ERROR_GEN_FAILURE
    case 0xC0000002u: return 120;        // ERROR_CALL_NOT_IMPLEMENTED
    case 0xC0000003u: return 122;        // ERROR_INSUFFICIENT_BUFFER (STATUS_INVALID_INFO_CLASS)
    case 0xC0000004u: return 24;         // ERROR_BAD_LENGTH
    case 0xC0000008u: return 6;          // ERROR_INVALID_HANDLE
    case 0xC000000Du: return 87;         // ERROR_INVALID_PARAMETER
    case 0xC0000010u: return 1;          // ERROR_INVALID_FUNCTION
    case 0xC0000011u: return 38;         // ERROR_HANDLE_EOF
    case 0xC0000017u: return 8;          // ERROR_NOT_ENOUGH_MEMORY
    case 0xC0000022u: return 5;          // ERROR_ACCESS_DENIED
    case 0xC0000023u: return 122;        // ERROR_INSUFFICIENT_BUFFER
    case 0xC0000033u: return 85;         // ERROR_INVALID_NAME
    case 0xC0000034u: return 2;          // ERROR_FILE_NOT_FOUND
    case 0xC0000035u: return 183;        // ERROR_ALREADY_EXISTS
    case 0xC000003Au: return 3;          // ERROR_PATH_NOT_FOUND
    case 0xC0000043u: return 32;         // ERROR_SHARING_VIOLATION
    case 0xC0000056u: return 5;          // ERROR_ACCESS_DENIED (delete pending)
    case 0xC000007Fu: return 112;        // ERROR_DISK_FULL
    case 0xC00000BAu: return 5;          // ERROR_ACCESS_DENIED (file is a directory)
    case 0xC00000BBu: return 50;         // ERROR_NOT_SUPPORTED
    case 0xC0000100u: return 995;        // ERROR_IO_INCOMPLETE? (STATUS_FILE_IS_A_DIRECTORY on some builds)
    case 0xC0000101u: return 145;        // ERROR_DIR_NOT_EMPTY
    case 0xC0000103u: return 267;        // ERROR_DIRECTORY
    case 0xC0000120u: return 995;        // ERROR_CANCELLED
    case 0xC000016Au: return 995;        // ERROR_CANCELLED
    case 0xC0000203u: return 145;        // ERROR_DIR_NOT_EMPTY (STATUS_DIRECTORY_NOT_EMPTY variant)
    case 0xC0000205u: return 123;        // ERROR_INVALID_NAME
    case 0xC0000225u: return 122;        // ERROR_INSUFFICIENT_BUFFER
    case 0xC000026Eu: return 995;        // ERROR_CANCELLED
    case 0xC000041Du: return 995;        // ERROR_CANCELLED
    default: return 31;                 // ERROR_GEN_FAILURE, which is what the real function answers
    }
}

// ----------------------------------------------------------------------------------------------------------
// time

extern "C" void __attribute__((stdcall)) RtlTimeToTimeFields(const struct XLargeInteger *Time,
                                                             XTimeFields *TimeFields)
{
    if (!Time || !TimeFields) return;
    memset(TimeFields, 0, sizeof *TimeFields);

    int64_t units = Time->QuadPart;
    if (units < 0) units = 0; // before 1601: the Xbox's clock never goes there, and a negative count
                              // would make the day-of-week arithmetic negative
    const uint32_t sub = (uint32_t)(units % KR_UNITS_PER_SECOND); // 100 ns within the second
    int64_t seconds = units / KR_UNITS_PER_SECOND;
    const int64_t days = seconds / 86400;
    int64_t sod = seconds % 86400;

    int32_t y;
    uint32_t m, d;
    kr_civil_from_days(days, &y, &m, &d);
    TimeFields->Year = (uint16_t)y;
    TimeFields->Month = (uint16_t)m;
    TimeFields->Day = (uint16_t)d;
    TimeFields->Weekday = kr_weekday_from_days(days);
    TimeFields->Hour = (uint16_t)(sod / 3600);
    sod %= 3600;
    TimeFields->Minute = (uint16_t)(sod / 60);
    TimeFields->Second = (uint16_t)(sod % 60);
    TimeFields->Milliseconds = (uint16_t)(sub / 10000);
}

extern "C" uint8_t __attribute__((stdcall)) RtlTimeFieldsToTime(const XTimeFields *TimeFields,
                                                                 struct XLargeInteger *Time)
{
    if (!TimeFields || !Time) return 0;
    // The Xbox refuses a field set it cannot represent rather than producing a time that is wrong; a
    // game that gets FALSE here is meant to notice.
    if (TimeFields->Month < 1 || TimeFields->Month > 12) return 0;
    if (TimeFields->Day < 1 || TimeFields->Day > 31) return 0;
    if (TimeFields->Hour > 23 || TimeFields->Minute > 59 || TimeFields->Second > 59) return 0;
    if (TimeFields->Milliseconds > 999) return 0;

    const int64_t days = kr_days_from_civil(TimeFields->Year, TimeFields->Month, TimeFields->Day);
    const int64_t seconds = days * 86400 + TimeFields->Hour * 3600 + TimeFields->Minute * 60 + TimeFields->Second;
    Time->QuadPart = seconds * KR_UNITS_PER_SECOND + (int64_t)TimeFields->Milliseconds * 10000LL;
    return 1;
}

// ----------------------------------------------------------------------------------------------------------
// critical sections

extern "C" void __attribute__((stdcall)) RtlInitializeCriticalSection(XRtlCriticalSection *CriticalSection)
{
    xk_initialize_critical_section(CriticalSection);
}

extern "C" void __attribute__((stdcall)) RtlEnterCriticalSection(XRtlCriticalSection *CriticalSection)
{
    xk_enter_critical_section(CriticalSection);
}

extern "C" void __attribute__((stdcall)) RtlLeaveCriticalSection(XRtlCriticalSection *CriticalSection)
{
    xk_leave_critical_section(CriticalSection);
}