// The handle space, and the one property it exists for.
//
// Two layers keep separate arenas -- files carry host fds and mounts, sync objects carry condition
// variables -- while the Xbox hands out handles from one process-wide table, so the game passes any
// handle to NtClose without saying what it is. If a file handle and an event handle could be the same
// integer, closing one would close the other's object. That is what the kind tag in the top four bits
// prevents, and this suite is what keeps the prevention honest.
//
// Everything here runs against a real temporary directory and real condition variables: the point being
// checked is that two live objects in two arenas never share a value.
#include "platform/posix/xfile.h"
#include "platform/posix/xhandle.h"
#include "platform/posix/xsync.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdlib.h>

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

// ---------------------------------------------------------------------------

static void test_encoding()
{
    printf("\nthe tag and the index\n");

    check_u32(xhandle_make(XHANDLE_SYNC, 0), 0x10000000u, "a sync handle with index 0 is tagged, not zero");
    check_u32(xhandle_make(XHANDLE_FILE, 0), 0x20000000u, "a file handle with index 0");
    check_u32(xhandle_index(xhandle_make(XHANDLE_LINK, 7)), 7, "the index survives the round trip");
    check_u32(xhandle_kind(xhandle_make(XHANDLE_THREAD, 5)), XHANDLE_THREAD, "and so does the kind");

    check(!xhandle_valid(0), "zero is not a handle");
    check(!xhandle_valid(XHANDLE_NONE_VALUE), "0xFFFFFFFF is not a handle");
    check(!xhandle_valid(xhandle_make(5, 1)), "kind 5 is not one anybody uses");
    check(xhandle_valid(xhandle_make(XHANDLE_SYNC, 0)), "a real handle is valid");

    // The invalid value has to stay out of range of every arena, or "not a handle" would look like a
    // valid entry.
    check(xhandle_index(XHANDLE_NONE_VALUE) > 100000, "and the invalid value's index is out of any arena's range");
    check_u32(XINVALID_HANDLE, XHANDLE_NONE_VALUE, "the sync layer's invalid handle is the same value");
    check_u32(XF_INVALID_HANDLE_VALUE, XHANDLE_NONE_VALUE, "and so is the file layer's");
}

// ---------------------------------------------------------------------------

static std::string g_root, g_game, g_hdd;

static void mk(const std::string &rel)
{
    std::string full = g_root + "/" + rel;
    std::string acc;
    for (size_t i = 0; i <= full.size(); i++) {
        if ((i == full.size() || full[i] == '/') && i && full[i - 1] != '/') mkdir(acc.c_str(), 0777);
        if (i < full.size()) acc += full[i];
    }
}

static XfHandle open_game_file(const char *rel, NTSTATUS *st)
{
    const std::string path = std::string("\\Device\\CdRom0\\") + rel;
    std::vector<char> buf(path.begin(), path.end());
    XAnsiString name{(uint16_t)path.size(), (uint16_t)path.size(), buf.data()};
    XObjectAttributes oa{XF_INVALID_HANDLE_VALUE, &name, 0};
    XfHandle h = XF_INVALID_HANDLE_VALUE;
    *st = xf_create_file(&h, XF_GENERIC_READ, &oa, nullptr, nullptr, 0, 0, XF_FILE_OPEN, 0);
    return h;
}

static void test_two_arenas()
{
    printf("\ntwo arenas, one value space\n");

    mk("game");
    mk("hdd");
    { // a file for the file arena to hold
        FILE *f = fopen((g_root + "/game/intro.vid").c_str(), "wb");
        fputs("INTROVIDEO0000", f);
        fclose(f);
    }
    g_game = g_root + "/game";
    g_hdd = g_root + "/hdd";
    xf_init(g_game, g_hdd);

    // Both arenas hand out low indices starting at 0 or 1, so the collision is real unless the tag
    // keeps them apart.
    XHandle event = XINVALID_HANDLE;
    check(xk_create_event(&event, 0 /* notification */, 0) == XK_SUCCESS, "an event was created");

    NTSTATUS st = XF_INVALID_HANDLE;
    const XfHandle file = open_game_file("intro.vid", &st);
    check(st == XF_SUCCESS, "a file was opened");

    check_u32(xhandle_index(event), 0, "the event's arena index is 0");
    check_u32(xhandle_index(file), 1, "the file's arena index is 1");
    check(event != file, "and the handles the game holds are not the same value");
    check_u32(xhandle_kind(event), XHANDLE_SYNC, "the event says which arena it came from");
    check_u32(xhandle_kind(file), XHANDLE_FILE, "and so does the file");

    // Neither layer may resolve the other's handle: this is what NtClose's dispatch relies on.
    check(xf_handle_desc(event) == "?", "the file layer does not recognise a sync handle");
    check(xk_wait_handle(file, 0, nullptr) == XK_INVALID_HANDLE, "and the sync layer does not recognise a file handle");
    check(xf_close(event) == XF_INVALID_HANDLE, "closing a sync handle as a file is refused");
    check(xk_close(file) == XK_INVALID_HANDLE, "and closing a file handle as a sync object is refused");

    // The refusal must not have closed either object: both are still live.
    XLargeInteger zero;
    zero.QuadPart = 0;
    char buf[16] = {};
    check(xf_read(file, buf, 4, &zero, nullptr) == XF_SUCCESS, "the file is still open after the refused close");
    check(std::string(buf) == "INTR", "and still readable");
    // The event was created unsignalled, so a wait on it must time out -- which is itself proof the
    // object behind the handle is the one that was created, and still live.
    XLargeInt expired;
    expired.QuadPart = -100000; // 10 ms, long gone
    check(xk_wait_handle(event, 0, &expired) == XK_TIMEOUT, "the event handle still refers to an unsignalled event");
    check(xk_set_event_handle(event, nullptr) == XK_SUCCESS, "and the event behind it can still be set");
    check(xk_wait_handle(event, 0, &expired) == XK_SUCCESS, "after which a wait on it succeeds");

    check_u32(xf_open_handle_count(), 1, "one file handle is live");
    check(xf_close(file) == XF_SUCCESS, "the file closes through its own layer");
    check_u32(xf_open_handle_count(), 0, "and the file arena is empty");
    check(xk_close(event) == XK_SUCCESS, "the event closes through its own layer");
}

int main()
{
    printf("BLACK loader -- the handle space\n");

    // A private temporary tree; removed on the way out.
    char tmpl[] = "/tmp/blacklinux-handle-XXXXXX";
    const char *dir = mkdtemp(tmpl);
    if (!dir) {
        printf("FATAL: cannot create a temporary directory\n");
        return 1;
    }
    g_root = dir;

    test_encoding();
    test_two_arenas();

    // Leave nothing behind: the tree is ours, and a stray Partition tree in /tmp is not.
    std::string cmd = "rm -rf '" + g_root + "'";
    if (system(cmd.c_str()) != 0) printf("  (warning: could not remove %s)\n", g_root.c_str());

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}