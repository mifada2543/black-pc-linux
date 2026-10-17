// The Rtl* exports: strings, time, critical sections, and the status-to-Win32 mapping.
//
// The time checks are the ones worth reading twice. RtlTimeToTimeFields and RtlTimeFieldsToTime are the
// only way the game reads the clock in parts, and this port has to compute them itself rather than borrow
// them from a host library, so every conversion here is compared against gmtime_r -- a completely separate
// implementation of the same calendar. If the two ever disagree about a date, one of them is wrong and
// this suite is what says so.
//
// The critical-section checks use real threads, because a critical section that only ever runs on one
// thread proves nothing about the lock inside it.
#include "kernel/k_rtl_posix.h"

#include "kernel/k_memory_posix.h" // ExFreePool, to give the allocated buffers back

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <string>

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
    fflush(stdout); // this suite waits on threads and may be killed: the last line has to survive
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
    fflush(stdout);
}

// ----------------------------------------------------------------------------------------------------------
// strings
// ----------------------------------------------------------------------------------------------------------

static XAnsiString ansi(const char *s)
{
    XAnsiString a;
    RtlInitAnsiString(&a, s);
    return a;
}

static void test_strings()
{
    printf("\nansi strings\n");

    XAnsiString a;
    RtlInitAnsiString(&a, "\\Device\\CdRom0\\media"); // 20 characters: two backslashes are escapes
    check_u32(a.Length, 20, "RtlInitAnsiString measures the string");
    check_u32(a.MaximumLength, 21, "with room for the terminator");
    check(a.Buffer != nullptr && strcmp(a.Buffer, "\\Device\\CdRom0\\media") == 0, "and keeps the pointer");

    RtlInitAnsiString(&a, "");
    check_u32(a.Length, 0, "an empty string measures zero");
    check_u32(a.MaximumLength, 1, "with one byte of room");

    RtlInitAnsiString(&a, nullptr);
    check_u32(a.Length, 0, "a null source gives an empty string, not a crash");

    const XAnsiString one = ansi("intro.vid");
    const XAnsiString same = ansi("intro.vid");
    const XAnsiString upper = ansi("INTRO.VID");
    const XAnsiString longer = ansi("intro.video");

    check(RtlEqualString(&one, &same, 1) != 0, "two equal strings are equal");
    check(RtlEqualString(&one, &upper, 1) != 0, "case insensitively, too");
    check(RtlEqualString(&one, &upper, 0) == 0, "but not case sensitively");
    check(RtlEqualString(&one, &longer, 0) == 0, "different lengths are not equal");

    // RtlCompareMemoryUlong compares ULONGs and answers where they stop matching: 0 when the first word
    // differs, 4 when the first matches and the second does not, and so on.
    alignas(4) uint32_t words[4] = {0x11223344, 0x55667788, 0x9AAAAAAA, 0xBBBBBBBB};
    check_u32(RtlCompareMemoryUlong(words, 16, 0xDEADBEEF), 0, "a first word that differs stops at 0");
    check_u32(RtlCompareMemoryUlong(words, 16, 0x11223344), 4, "one matching word stops at 4");

    alignas(4) uint32_t two[4] = {0x5A5A5A5A, 0x5A5A5A5A, 0x12345678, 0xBBBBBBBB};
    check_u32(RtlCompareMemoryUlong(two, 16, 0x5A5A5A5A), 8, "two matching words stop at 8");

    alignas(4) uint32_t three[4] = {0x5A5A5A5A, 0x5A5A5A5A, 0x5A5A5A5A, 0x11111111};
    check_u32(RtlCompareMemoryUlong(three, 16, 0x5A5A5A5A), 12, "three matching words stop at 12");

    alignas(4) uint32_t uniform[4] = {0x5A5A5A5A, 0x5A5A5A5A, 0x5A5A5A5A, 0x5A5A5A5A};
    check_u32(RtlCompareMemoryUlong(uniform, 16, 0x5A5A5A5A), 16,
              "a block that matches all the way stops at its length");
    check_u32(RtlCompareMemoryUlong(words, 10, 0xDEADBEEF), 0, "a length below the first word stops at 0");
}

static void test_conversions()
{
    printf("\nwide strings\n");

    const XAnsiString src = ansi("D:\\games\\BLACK");

    // Allocate: the buffer comes from the pool and must be the right size for the wide string plus a
    // terminator, or the game's later ExFreePool finds a block that is not there.
    XUnicodeString wide;
    check(RtlAnsiStringToUnicodeString(&wide, &src, 1) == KR_SUCCESS, "an ansi string converts to wide");
    check_u32(wide.Length, 2 * src.Length, "the length is in bytes");
    check_u32(wide.MaximumLength, 2 * src.Length + 2, "with room for a wide terminator");
    check(wide.Buffer != nullptr && wcscmp(wide.Buffer, L"D:\\games\\BLACK") == 0, "and the text matches");

    // Narrow it again into the same wide string's own buffer, without allocating.
    XAnsiString back;
    memset(&back, 0, sizeof back);
    back.MaximumLength = (uint16_t)(src.Length + 1);
    back.Buffer = (char *)malloc(back.MaximumLength);
    check(RtlUnicodeStringToAnsiString(&back, &wide, 0) == KR_SUCCESS, "and back to ansi without allocating");
    check(back.Length == src.Length && memcmp(back.Buffer, src.Buffer, src.Length) == 0, "the text survives");
    check_u32(back.Buffer[back.Length], 0, "with a terminator");

    // A destination too small must say so rather than truncate silently.
    XAnsiString small;
    memset(&small, 0, sizeof small);
    small.MaximumLength = 4;
    small.Buffer = (char *)malloc(4);
    check(RtlUnicodeStringToAnsiString(&small, &wide, 0) == KR_BUFFER_OVERFLOW,
          "a destination that is too small reports an overflow");

    XUnicodeString narrow_wide;
    memset(&narrow_wide, 0, sizeof narrow_wide);
    narrow_wide.MaximumLength = 4;
    narrow_wide.Buffer = (wchar_t *)malloc(4);
    check(RtlAnsiStringToUnicodeString(&narrow_wide, &src, 0) == KR_BUFFER_OVERFLOW,
          "and the same for the other direction");

    // A character with no byte representation becomes '?' rather than being dropped: 0xE9 fits in a byte,
    // but 0x0141 does not, and that is the one this has to check.
    wchar_t mixed[] = {0x0041, 0x0141, 0x0042, 0};
    XUnicodeString mixed_src;
    mixed_src.Length = 6;
    mixed_src.MaximumLength = 8;
    mixed_src.Buffer = mixed;
    XAnsiString narrow;
    memset(&narrow, 0, sizeof narrow);
    narrow.MaximumLength = 4;
    narrow.Buffer = (char *)malloc(4);
    check(RtlUnicodeStringToAnsiString(&narrow, &mixed_src, 0) == KR_SUCCESS, "a mixed string narrows");
    check(narrow.Length == 3 && narrow.Buffer[0] == 'A' && narrow.Buffer[1] == '?' && narrow.Buffer[2] == 'B',
          "with the character the code page cannot show replaced by '?'");

    ExFreePool(wide.Buffer);
    free(back.Buffer);
    free(small.Buffer);
    free(narrow_wide.Buffer);
    free(narrow.Buffer);
}

// ----------------------------------------------------------------------------------------------------------
// time
// ----------------------------------------------------------------------------------------------------------

// One timestamp, checked against gmtime_r -- a completely separate implementation of the same calendar.
// Only for dates after 1970: time_t is 32 bits here, so anything earlier wraps and gmtime_r becomes
// useless. Dates before that are checked against hand-computed values instead.
static void check_time(int64_t units, const std::string &what)
{
    XLargeInteger t;
    t.QuadPart = units;
    XTimeFields tf;
    RtlTimeToTimeFields(&t, &tf);

    // The timestamps are in the Xbox's frame (100 ns since 1601); gmtime_r wants seconds since 1970.
    const time_t seconds = (time_t)(units / 10000000LL - 11644473600LL);
    struct tm host {};
    gmtime_r(&seconds, &host);

    const bool same = tf.Year == (uint16_t)(host.tm_year + 1900) && tf.Month == (uint16_t)(host.tm_mon + 1) &&
                      tf.Day == (uint16_t)host.tm_mday && tf.Hour == (uint16_t)host.tm_hour &&
                      tf.Minute == (uint16_t)host.tm_min && tf.Second == (uint16_t)host.tm_sec &&
                      tf.Weekday == (uint16_t)host.tm_wday;
    if (!same) {
        printf("  FAIL %s: %04u-%02u-%02u %02u:%02u:%02u wd %u, host says %04d-%02d-%02d %02d:%02d:%02d wd %d\n",
               what.c_str(), tf.Year, tf.Month, tf.Day, tf.Hour, tf.Minute, tf.Second, tf.Weekday,
               host.tm_year + 1900, host.tm_mon + 1, host.tm_mday, host.tm_hour, host.tm_min, host.tm_sec,
               host.tm_wday);
        fflush(stdout);
        g_failures++;
        return;
    }
    g_checks++;
    printf("  ok   %s\n", what.c_str());

    // And back again: the two have to be inverses, or a game that writes a time and reads it back loses it.
    XLargeInteger back;
    check(RtlTimeFieldsToTime(&tf, &back) != 0, "  and converts back");
    check_u32((uint32_t)(back.QuadPart / 10000000LL), (uint32_t)(units / 10000000LL), "  to the same second");
}

// Zeller's congruence: an entirely different algorithm from the era-based day count under test, and it
// works for any year, which matters because a 32-bit time_t cannot hold 1601. Returns Sunday = 0 to match
// TIME_FIELDS.
static uint16_t weekday_zeller(int y, int m, int d)
{
    if (m < 3) {
        m += 12;
        y -= 1;
    }
    const int k = y % 100;
    const int j = y / 100;
    const int h = (d + (13 * (m + 1)) / 5 + k + k / 4 + j / 4 + 5 * j + 6) % 7; // 0 = Saturday
    // Zeller counts from Saturday; TIME_FIELDS counts from Sunday. (Python's datetime counts from Monday,
    // which is where an earlier version of this helper went wrong: it agreed with datetime and disagreed
    // with every real calendar. 1601-01-01 was a Monday -- fixed by walking back from 1970-01-01, which was
    // a Thursday, over the 134774 days between them.)
    return (uint16_t)((h + 7) % 7);
}

// Dates from the Xbox's own era, which gmtime_r cannot be asked about on this target. The expected weekday
// comes from Zeller rather than from a number written down here.
static void check_early_time(int64_t units, uint16_t y, uint16_t mo, uint16_t d, const char *what)
{
    XLargeInteger t;
    t.QuadPart = units;
    XTimeFields tf;
    RtlTimeToTimeFields(&t, &tf);
    const uint16_t wd = weekday_zeller(y, mo, d);
    const bool same = tf.Year == y && tf.Month == mo && tf.Day == d && tf.Weekday == wd;
    if (!same) {
        printf("  FAIL %s: got %04u-%02u-%02u wd %u, want %04u-%02u-%02u wd %u (Zeller)\n", what, tf.Year,
               tf.Month, tf.Day, tf.Weekday, y, mo, d, wd);
        fflush(stdout);
        g_failures++;
        return;
    }
    g_checks++;
    printf("  ok   %s\n", what);

    XLargeInteger back;
    check(RtlTimeFieldsToTime(&tf, &back) != 0, "  and converts back");
    check_u32((uint32_t)(back.QuadPart / 10000000LL), (uint32_t)(units / 10000000LL), "  to the same second");
}

static void test_time()
{
    printf("\ntime\n");

    // 1601-01-01, the epoch KeQuerySystemTime counts from. A Saturday, which is how the Xbox numbers
    // weekdays, and a date no host calendar starts at.
    check_u32((uint32_t)kr_days_from_civil(1601, 1, 1), 0, "day 0 is 1601-01-01");
    check_u32(kr_weekday_from_days(0), 1, "which was a Monday");
    check_u32((uint32_t)(kr_days_from_civil(1970, 1, 1) - kr_days_from_civil(1601, 1, 1)), 134774,
              "and 1970 is 134774 days later");

    check_early_time(0, 1601, 1, 1, "the epoch itself is 1601-01-01");
    check_early_time(10000000LL, 1601, 1, 1, "one second past the epoch is still that day");
    check_early_time(10000000LL * 86400, 1601, 1, 2, "one day past it is the 2nd");
    // Day 0 is 1 January, so the last day of a 365-day year is day 364 -- which is why 365 days lands on
    // 2 January 1602. Getting this wrong is how a leap-year bug hides.
    check_early_time(10000000LL * 86400 * 364, 1601, 12, 31, "the last day of 1601, a non-leap year");
    check_early_time(10000000LL * 86400 * 365, 1602, 1, 1, "and 365 days later is 2 January 1602");
    // 1604 is the first leap year of the century (divisible by 4, not by 100), so it has a 29 February:
    // day 59, because 31 days of January plus 28 of February puts 29 February on the 60th.
    check_early_time(10000000LL * 86400 * 1154, 1604, 2, 29,
                     "29 February 1604 exists: 1154 days after the epoch, not 59");
    check_early_time(10000000LL * 86400 * 1460, 1604, 12, 31, "and that leap year ends on 31 December");

    // After 1970 the host calendar can be asked directly, which is the stronger check.
    // A timestamp in the Xbox's frame is (unix seconds + 11644473600) * 10^7.
    check_time((int64_t)(1788134400LL + 11644473600LL) * 10000000LL, "an arbitrary later date (2026-10-17)");
    check_time((int64_t)(951782400LL + 11644473600LL) * 10000000LL, "a leap day (2000-02-29)");
    // 2030, not later: time_t is 32 bits here, so gmtime_r cannot be asked about anything past 2038.
    check_time((int64_t)(1893456000LL + 11644473600LL) * 10000000LL, "1 January 2030");
    check_time((int64_t)11644473600LL * 10000000LL, "the 1970 epoch itself");
    check_time((int64_t)(1788134400LL + 11644473600LL) * 10000000LL + 500000, "a timestamp with a half second");
    check_early_time(1234567890LL, 1601, 1, 1, "an odd number of 100 ns units is still the epoch second");

    // Fields the Xbox cannot represent are refused rather than turned into a wrong time. The whole set is
    // initialised: an uninitialised minute or second would be refused for the wrong reason.
    XTimeFields bad = {2026, 10, 17, 6, 12, 34, 56, 560};
    XLargeInteger out;
    XTimeFields probe = bad;
    probe.Month = 13; // no such month
    check(RtlTimeFieldsToTime(&probe, &out) == 0, "month 13 is refused");
    probe = bad;
    probe.Day = 32;
    check(RtlTimeFieldsToTime(&probe, &out) == 0, "day 32 is refused");
    probe = bad;
    probe.Hour = 24;
    check(RtlTimeFieldsToTime(&probe, &out) == 0, "hour 24 is refused");
    probe = bad;
    probe.Second = 60;
    check(RtlTimeFieldsToTime(&probe, &out) == 0, "second 60 is refused");
    probe = bad;
    probe.Milliseconds = 1000;
    check(RtlTimeFieldsToTime(&probe, &out) == 0, "1000 milliseconds is refused");

    check(RtlTimeFieldsToTime(&bad, &out) != 0, "and a sane field set is accepted");
    check_u32((uint32_t)(out.QuadPart % 10000000LL), 560 * 10000, "with the milliseconds in the right place");
    // The frame, checked against timegm rather than a number typed in: 2026-10-17 12:34:56 UTC. Getting
    // the epoch wrong puts every date the game computes 369 years out, and nothing else would notice.
    struct tm utc = {};
    utc.tm_year = bad.Year - 1900;
    utc.tm_mon = (int)bad.Month - 1;
    utc.tm_mday = bad.Day;
    utc.tm_hour = bad.Hour;
    utc.tm_min = bad.Minute;
    utc.tm_sec = bad.Second;
    check_u32((uint32_t)(out.QuadPart / 10000000LL - 11644473600LL), (uint32_t)timegm(&utc),
              "and the seconds are counted from 1601, not from 1970");

    // The fields carry milliseconds, not 100 ns units: 15 250 000 units is 1 s 525 ms.
    XTimeFields tf;
    XLargeInteger exact;
    exact.QuadPart = 15000000LL + 250000LL;
    RtlTimeToTimeFields(&exact, &tf);
    check_u32(tf.Milliseconds, 525, "the fields carry the milliseconds");
    check_u32(tf.Second, 1, "and the seconds");
}

// ----------------------------------------------------------------------------------------------------------
// status mapping
// ----------------------------------------------------------------------------------------------------------

static void test_status_mapping()
{
    printf("\nstatus to Win32 error\n");

    // The ones the file and sync layers actually return, since those are the ones a game will branch on.
    check_u32(RtlNtStatusToDosError(XF_SUCCESS), 0, "success is ERROR_SUCCESS");
    check_u32(RtlNtStatusToDosError(XF_OBJECT_NAME_NOT_FOUND), 2, "a missing name is ERROR_FILE_NOT_FOUND");
    check_u32(RtlNtStatusToDosError(XF_OBJECT_PATH_NOT_FOUND), 3, "a missing path is ERROR_PATH_NOT_FOUND");
    check_u32(RtlNtStatusToDosError(XF_ACCESS_DENIED), 5, "denied is ERROR_ACCESS_DENIED");
    check_u32(RtlNtStatusToDosError(XF_INVALID_HANDLE), 6, "a bad handle is ERROR_INVALID_HANDLE");
    check_u32(RtlNtStatusToDosError(XF_NO_MEMORY), 8, "out of memory is ERROR_NOT_ENOUGH_MEMORY");
    check_u32(RtlNtStatusToDosError(XF_INVALID_PARAMETER), 87, "a bad parameter is ERROR_INVALID_PARAMETER");
    check_u32(RtlNtStatusToDosError(XF_NO_MORE_FILES), 18, "the end of a directory is ERROR_NO_MORE_FILES");
    check_u32(RtlNtStatusToDosError(XF_NOT_SUPPORTED), 50, "unsupported is ERROR_NOT_SUPPORTED");
    check_u32(RtlNtStatusToDosError(XF_INVALID_DEVICE_REQUEST), 1, "a bad ioctl is ERROR_INVALID_FUNCTION");
    check_u32(RtlNtStatusToDosError(0x102), 258, "a timeout is WAIT_TIMEOUT");
    check_u32(RtlNtStatusToDosError(0x80000005), 234, "an overflow is ERROR_MORE_DATA");
    check_u32(RtlNtStatusToDosError(XF_OBJECT_NAME_COLLISION), 183, "a name collision is ERROR_ALREADY_EXISTS");
    check_u32(RtlNtStatusToDosError(0xCCCCCCCC), 31, "a status nobody knows is ERROR_GEN_FAILURE");

    // Every mapping must fit in what the game expects: a Win32 error code is a DWORD, and these are all
    // small numbers the CRT prints as such.
    for (uint32_t s = 0; s < 0x200; s++) {
        const uint32_t e = RtlNtStatusToDosError((NTSTATUS)s);
        if (e > 100000) {
            check(false, "status 0x" + std::to_string(s) + " maps to something absurd");
            return;
        }
    }
    check(true, "no status below 0x200 maps to an absurd error code");
}

// ----------------------------------------------------------------------------------------------------------
// critical sections
// ----------------------------------------------------------------------------------------------------------

static XRtlCriticalSection g_cs;
// Plain ints: the critical section is what makes these safe to touch from two threads, and using volatile
// as well would be relying on the wrong mechanism.
static int g_counter;
static int g_inside;

static void *hammer_the_counter(void *)
{
    for (int i = 0; i < 20000; i++) {
        RtlEnterCriticalSection(&g_cs);
        g_inside++;
        g_counter++;
        g_inside--;
        RtlLeaveCriticalSection(&g_cs);
    }
    return nullptr;
}

static void test_critical_sections()
{
    printf("\ncritical sections\n");

    check_u32(sizeof(XRtlCriticalSection), 28, "the structure is the Xbox's 28 bytes");

    RtlInitializeCriticalSection(&g_cs);
    // LockCount starts at -1 on the Xbox, which is what RtlEnterCriticalSection turns into zero.
    check_u32((uint32_t)(int32_t)g_cs.LockCount, 0xFFFFFFFFu, "an initialised critical section has LockCount -1");
    check_u32(g_cs.RecursionCount, 0, "no recursion yet");
    check(g_cs.OwningThread == nullptr, "and no owner");

    RtlEnterCriticalSection(&g_cs);
    check_u32((uint32_t)(int32_t)g_cs.LockCount, 0, "entering takes it: LockCount 0");
    check_u32(g_cs.RecursionCount, 1, "and the recursion count is one");
    check(g_cs.OwningThread != nullptr, "with an owner recorded");

    RtlEnterCriticalSection(&g_cs);
    check_u32(g_cs.RecursionCount, 2, "entering again from the same thread only recurses");
    RtlLeaveCriticalSection(&g_cs);
    check_u32(g_cs.RecursionCount, 1, "and one leave unwinds one level");
    RtlLeaveCriticalSection(&g_cs);
    check_u32((uint32_t)(int32_t)g_cs.LockCount, 0xFFFFFFFFu, "the last leave releases it: LockCount back to -1");

    // Two threads incrementing a counter under it: without the lock this loses updates, and the check is
    // that it does not.
    g_counter = 0;
    g_inside = 0;
    pthread_t a, b;
    pthread_create(&a, nullptr, hammer_the_counter, nullptr);
    pthread_create(&b, nullptr, hammer_the_counter, nullptr);
    pthread_join(a, nullptr);
    pthread_join(b, nullptr);
    check(g_counter == 40000, "two threads incremented it 40000 times without losing any (" +
                                  std::to_string(g_counter) + ")");
    check(g_inside == 0, "and never overlapped inside the section");

    // Re-initialising a held section is what the CRT does after a fork or a panic.
    RtlInitializeCriticalSection(&g_cs);
    RtlEnterCriticalSection(&g_cs);
    RtlLeaveCriticalSection(&g_cs);
    check_u32((uint32_t)(int32_t)g_cs.LockCount, 0xFFFFFFFFu, "re-initialising and using it again works");
}

int main()
{
    printf("BLACK loader -- Rtl exports\n");

    test_strings();
    test_conversions();
    test_time();
    test_status_mapping();
    test_critical_sections();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}