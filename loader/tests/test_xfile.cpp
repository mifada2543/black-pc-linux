// Tests for the POSIX Xbox file layer.
//
// These run against a real temporary filesystem tree standing in for the game's disc and hard disk,
// so path resolution, the mount table, read-only media, FATX volume numbers and raw volumes are all
// exercised for real rather than mocked.
#include "platform/posix/xfile.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

static int g_failures;
static int g_checks;

static void check(bool cond, const char *what)
{
    g_checks++;
    if (cond) { printf("  ok   %s\n", what); }
    else { g_failures++; printf("  FAIL %s\n", what); }
}

static std::string g_root;
static std::string g_game, g_hdd;

static void mk(const std::string &p)
{
    std::string full = g_root + "/" + p;
    std::string acc;
    for (size_t i = 0; i <= full.size(); i++) {
        if (i == full.size() || full[i] == '/') {
            if (i && full[i - 1] != '/') mkdir(acc.c_str(), 0777);
        }
        if (i < full.size()) acc += full[i];
    }
}

static void put(const std::string &rel, const std::string &data)
{
    std::string full = g_root + "/" + rel;
    FILE *f = fopen(full.c_str(), "wb");
    if (!f) { printf("  FAIL cannot create %s\n", full.c_str()); g_failures++; return; }
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
}

static const char *kIntro = "INTROVIDEO0000"; // 14 bytes

static XfHandle open_at(const std::string &path, uint32_t access, uint32_t disp, uint32_t opts, NTSTATUS *st = nullptr)
{
    std::vector<char> buf(path.begin(), path.end());
    XAnsiString name{(uint16_t)path.size(), (uint16_t)path.size(), buf.data()};
    XObjectAttributes oa{XF_INVALID_HANDLE_VALUE, &name, 0};
    XfHandle h = XF_INVALID_HANDLE_VALUE;
    NTSTATUS s = xf_create_file(&h, access, &oa, nullptr, nullptr, 0, 0, disp, opts);
    if (st) *st = s;
    return h;
}

static void add_links()
{
    xf_add_symlink("\\??\\D:", "\\Device\\CdRom0");
    xf_add_symlink("\\??\\E:", "\\Device\\Harddisk0\\Partition1");
    xf_add_symlink("\\??\\C:", "\\Device\\Harddisk0\\Partition2");
    xf_add_symlink("\\??\\X:", "\\Device\\Harddisk0\\Partition3");
    xf_add_symlink("\\??\\Y:", "\\Device\\Harddisk0\\Partition4");
    xf_add_symlink("\\??\\Z:", "\\Device\\Harddisk0\\Partition5");
}

// ---------------------------------------------------------------------------

static void test_resolution_and_read()
{
    printf("\npath resolution and reads\n");

    // Drive letter -> device -> host folder.
    XfHandle h = open_at("\\??\\D:\\media\\intro.xmv", XF_GENERIC_READ, XF_FILE_OPEN, 0);
    check(h != XF_INVALID_HANDLE_VALUE, "opened \\??\\D:\\media\\intro.xmv");
    check(xf_handle_desc(h) == "\\Device\\CdRom0\\media\\intro.xmv",
          "the handle remembers the canonical device path");
    if (h != XF_INVALID_HANDLE_VALUE) {
        char buf[16];
        XIoStatusBlock iosb{};
        // Read exactly the file's length: asking for more must report END_OF_FILE, not a short success.
        NTSTATUS s = xf_read(h, buf, 14, nullptr, &iosb);
        check(XF_NT_OK(s) && memcmp(buf, kIntro, 14) == 0, "read the file's contents");
        check(iosb.Information == 14, "IO_STATUS_BLOCK reports the byte count");
        NTSTATUS es = xf_read(h, buf, sizeof buf, nullptr, &iosb);
        check(es == XF_END_OF_FILE && iosb.Information == 0, "reading past the end reports END_OF_FILE");
        xf_close(h);
    }

    // Case-insensitive, like the Xbox object manager. This is not free on Linux: the host filesystem
    // is case-sensitive, so the layer has to match each component itself.
    XfHandle h2 = open_at("\\device\\cdrom0\\MEDIA\\INTRO.XMV", XF_GENERIC_READ, XF_FILE_OPEN, 0);
    check(h2 != XF_INVALID_HANDLE_VALUE, "paths are case-insensitive even though the host filesystem is not");
    if (h2 != XF_INVALID_HANDLE_VALUE) xf_close(h2);

    // No such file -> OBJECT_PATH_NOT_FOUND, not a generic failure.
    NTSTATUS s;
    open_at("\\??\\D:\\media\\nope.xmv", XF_GENERIC_READ, XF_FILE_OPEN, 0, &s);
    check(s == XF_OBJECT_PATH_NOT_FOUND, "a missing file gives OBJECT_PATH_NOT_FOUND");

    // An unmapped device must not fall through to the host filesystem.
    open_at("\\??\\Q:\\secret", XF_GENERIC_READ, XF_FILE_OPEN, 0, &s);
    check(s == XF_OBJECT_PATH_NOT_FOUND, "an unmapped drive letter is refused");

    // Relative to a directory handle (XAPI does this constantly).
    XfHandle dir = open_at("\\??\\D:\\media", XF_FILE_LIST_DIRECTORY, XF_FILE_OPEN, XF_DIRECTORY_FILE, &s);
    check(dir != XF_INVALID_HANDLE_VALUE, "opened a directory");
    if (dir != XF_INVALID_HANDLE_VALUE) {
        XAnsiString rel{9, 9, (char *)"intro.xmv"};
        XObjectAttributes oa{dir, &rel, 0};
        XfHandle child = XF_INVALID_HANDLE_VALUE;
        NTSTATUS rs = xf_open_file(&child, XF_GENERIC_READ, &oa, 0, 0);
        check(XF_NT_OK(rs) && child != XF_INVALID_HANDLE_VALUE, "resolved a name relative to a directory handle");
        if (child != XF_INVALID_HANDLE_VALUE) xf_close(child);
        xf_close(dir);
    }

    // Escaping the mount must not reach the host: ".." has to be contained.
    XAnsiString up{12, 12, (char *)"..\\..\\..\\etc\\passwd"};
    XObjectAttributes oa2{XF_INVALID_HANDLE_VALUE, &up, 0};
    XfHandle esc = XF_INVALID_HANDLE_VALUE;
    NTSTATUS es = xf_open_file(&esc, XF_GENERIC_READ, &oa2, 0, 0);
    check(es == XF_OBJECT_PATH_NOT_FOUND || esc == XF_INVALID_HANDLE_VALUE,
          "a path with .. cannot escape the mount");
    if (esc != XF_INVALID_HANDLE_VALUE) xf_close(esc);
}

static void test_readonly_media()
{
    printf("\nread-only media (the DVD)\n");
    NTSTATUS s;
    XfHandle h = open_at("\\??\\D:\\media\\intro.xmv", XF_GENERIC_WRITE | XF_GENERIC_READ, XF_FILE_OVERWRITE_IF, 0, &s);
    check(s == XF_ACCESS_DENIED && h == XF_INVALID_HANDLE_VALUE, "a write open on the DVD is ACCESS_DENIED");

    open_at("\\??\\D:\\brand_new.bin", XF_GENERIC_WRITE, XF_FILE_CREATE, 0, &s);
    check(s == XF_ACCESS_DENIED, "creating a file on the DVD is ACCESS_DENIED");

    // The partitions are writable, so the save game works.
    XfHandle w = open_at("\\??\\E:\\UDATA\\save.bin", XF_GENERIC_WRITE | XF_GENERIC_READ, XF_FILE_OPEN_IF, 0, &s);
    check(XF_NT_OK(s) && w != XF_INVALID_HANDLE_VALUE, "the E: save partition is writable");
    if (w != XF_INVALID_HANDLE_VALUE) {
        const char payload[] = "SAVEDATA";
        XIoStatusBlock iosb{};
        check(XF_NT_OK(xf_write(w, payload, 8, nullptr, &iosb)) && iosb.Information == 8, "wrote 8 bytes");
        xf_close(w);
    }
}

static void test_directory_enumeration()
{
    printf("\ndirectory enumeration\n");
    XfHandle dir = open_at("\\??\\D:\\media", XF_FILE_LIST_DIRECTORY, XF_FILE_OPEN, XF_DIRECTORY_FILE);
    if (dir == XF_INVALID_HANDLE_VALUE) { check(false, "opened \\Device\\CdRom0\\media"); return; }

    std::vector<std::string> names;
    for (int guard = 0; guard < 32; guard++) {
        alignas(8) uint8_t buf[512];
        XIoStatusBlock iosb{};
        NTSTATUS s = xf_query_directory(dir, buf, sizeof buf, "*.*", guard == 0, &iosb);
        if (s == XF_NO_MORE_FILES) break;
        if (!XF_NT_OK(s)) { check(false, "directory query succeeded"); break; }
        auto *e = (XFileDirectoryInformation *)buf;
        names.push_back(std::string(e->FileName, e->FileNameLength));
        if (e->NextEntryOffset != 0) check(false, "one entry per call (NextEntryOffset is zero)");
    }
    check(names.size() == 3, "three entries listed (the . and .. are skipped)");
    printf("       listed %zu:", names.size());
    for (auto &n : names) printf(" '%s'", n.c_str());
    printf("\n");
    bool has_intro = false, has_sub = false, has_dotdot = false, has_dot = false;
    for (auto &n : names) {
        if (n == "intro.xmv") has_intro = true;
        if (n == "sub") has_sub = true;
        if (n == ".") has_dot = true;
        if (n == "..") has_dotdot = true;
    }
    check(has_intro && has_sub, "the right entries came back, as 8-bit names");
    check(!has_dot && !has_dotdot, "\".\" and \"..\" are not reported");

    // RestartScan must rewind.
    uint8_t buf[512];
    XIoStatusBlock iosb{};
    NTSTATUS s = xf_query_directory(dir, buf, sizeof buf, "*.*", true, &iosb);
    auto *e = (XFileDirectoryInformation *)buf;
    check(XF_NT_OK(s) && std::string(e->FileName, e->FileNameLength) == names.front(),
          "RestartScan rewinds to the first entry");

    // A buffer that cannot hold the name must report BUFFER_OVERFLOW with the bytes it did copy,
    // not scribble past the end. RestartScan so the entry is known to be "intro.xmv" (10 chars);
    // 68 bytes leaves the 64-byte header plus 4 name characters.
    uint8_t tiny[68];
    NTSTATUS bs = xf_query_directory(dir, tiny, sizeof tiny, "*.*", true, &iosb);
    const auto *te = (const XFileDirectoryInformation *)tiny;
    check(bs == XF_BUFFER_OVERFLOW && iosb.Information == 68, "a short buffer yields BUFFER_OVERFLOW with the copied length");
    check(te->FileNameLength == 9 && memcmp(te->FileName, "intr", 4) == 0,
          "the truncated name is copied, 4 bytes of it, and FileNameLength still says 9");

    xf_close(dir);
}

static void test_volume_information()
{
    printf("\nvolume information (FATX-shaped)\n");
    // A bare device path is the raw volume, as on Windows (k_file.cpp: r.raw_volume = r.rest.empty()).
    // A trailing backslash makes it the directory itself.
    XfHandle part = open_at("\\Device\\Harddisk0\\Partition1\\", XF_FILE_LIST_DIRECTORY, XF_FILE_OPEN, XF_DIRECTORY_FILE);
    if (part == XF_INVALID_HANDLE_VALUE) { check(false, "opened Partition1"); return; }

    uint8_t buf[64];
    XIoStatusBlock iosb{};
    NTSTATUS s = xf_query_volume(part, buf, sizeof buf, XF_FileFsSizeInformation, &iosb);
    check(XF_NT_OK(s) && iosb.Information == 24, "FileFsSizeInformation returned 24 bytes");
    uint64_t total_clusters = *(uint64_t *)(buf + 0);
    uint32_t sectors = *(uint32_t *)(buf + 16);
    uint32_t bps = *(uint32_t *)(buf + 20);
    check(bps == 512, "BytesPerSector is 512");
    check(sectors == 32, "SectorsPerAllocationUnit is 32 (16 KB clusters)");
    // kPartitions[1].size = 0x1312D6000; /0x4000 = 131876 clusters.
    check(total_clusters == 0x1312D6000ull / 0x4000ull, "total clusters match the emulated partition size");

    uint8_t attr[32];
    s = xf_query_volume(part, attr, sizeof attr, XF_FileFsAttributeInformation, &iosb);
    check(XF_NT_OK(s) && strcmp((char *)(attr + 12), "FATX") == 0, "the HDD reports FATX");

    uint8_t vol[32];
    s = xf_query_volume(part, vol, sizeof vol, XF_FileFsVolumeInformation, &iosb);
    check(XF_NT_OK(s) && *(uint32_t *)(vol + 8) == 0x0B1AC001u, "the HDD volume serial carries the partition number");

    s = xf_query_volume(part, buf, 8, XF_FileFsSizeInformation, &iosb);
    check(s == XF_BUFFER_TOO_SMALL, "a short buffer reports BUFFER_TOO_SMALL");

    xf_close(part);

    // The DVD reports UDF, which is what it really is.
    XfHandle dvd = open_at("\\Device\\CdRom0\\", XF_FILE_LIST_DIRECTORY, XF_FILE_OPEN, XF_DIRECTORY_FILE);
    if (dvd != XF_INVALID_HANDLE_VALUE) {
        uint8_t a2[32];
        xf_query_volume(dvd, a2, sizeof a2, XF_FileFsAttributeInformation, &iosb);
        check(XF_NT_OK(iosb.Status) && strcmp((char *)(a2 + 12), "UDF") == 0, "the DVD reports UDF, not FATX");
        xf_close(dvd);
    }
}

static void test_raw_volume()
{
    printf("\nraw volume (the whole disk)\n");
    NTSTATUS s;
    XfHandle raw = open_at("\\Device\\Harddisk0\\Partition0", XF_GENERIC_READ, XF_FILE_OPEN, 0, &s);
    check(raw != XF_INVALID_HANDLE_VALUE, "Partition0 opened as a raw device");
    if (raw == XF_INVALID_HANDLE_VALUE) return;

    // Write a sector, then read it back by absolute sector offset.
    uint8_t sector[512];
    memset(sector, 0xA5, sizeof sector);
    XLargeInteger at{0};
    XIoStatusBlock iosb{};
    check(XF_NT_OK(xf_write(raw, sector, sizeof sector, &at, &iosb)), "wrote sector 0");

    uint8_t back[512];
    XLargeInteger at0{0};
    check(XF_NT_OK(xf_read(raw, back, sizeof back, &at0, &iosb)), "read sector 0 back");
    check(back[0] == 0xA5 && back[511] == 0xA5, "the sector came back intact");

    // Reading far past what was written must give zeroes, not fail: on the Xbox an unwritten sector
    // is simply zero, and the game reads the cache partitions this way.
    uint8_t far[512];
    XLargeInteger way_out{};
    way_out.QuadPart = (int64_t)0x1000000 * 512; // 1 GB in
    memset(far, 0xCC, sizeof far);
    NTSTATUS rs = xf_read(raw, far, sizeof far, &way_out, &iosb);
    check(XF_NT_OK(rs), "reading past the end of a raw volume succeeds");
    bool all_zero = true;
    for (uint32_t i = 0; i < sizeof far; i++)
        if (far[i]) all_zero = false;
    check(all_zero, "the unwritten sector reads as zeroes");

    // Geometry and partition info are what the game queries before touching the save partition.
    uint8_t geo[32];
    NTSTATUS gs = xf_device_io_control(raw, 0x70000, geo, sizeof geo, &iosb);
    check(XF_NT_OK(gs) && *(uint32_t *)geo == 512, "IOCTL_DISK_GET_DRIVE_GEOMETRY reports 512 bytes/sector");
    uint8_t part[16];
    NTSTATUS ps = xf_device_io_control(raw, 0x74004, part, sizeof part, &iosb);
    check(XF_NT_OK(ps) && *(uint32_t *)(part + 4) == 0, "IOCTL_DISK_GET_PARTITION_INFO starts at sector 0 for the whole disk");
    NTSTATUS us = xf_device_io_control(raw, 0xDEAD, part, sizeof part, &iosb);
    check(us == XF_INVALID_DEVICE_REQUEST, "an unknown IOCTL is refused");

    xf_close(raw);
}

static void test_handles()
{
    printf("\nhandles\n");
    uint32_t before = xf_open_handle_count();
    XfHandle a = open_at("\\??\\D:\\media\\intro.xmv", XF_GENERIC_READ, XF_FILE_OPEN, 0);
    check(xf_open_handle_count() == before + 1, "an open adds a handle");

    XfHandle dup = XF_INVALID_HANDLE_VALUE;
    check(XF_NT_OK(xf_duplicate(a, &dup, 0)) && dup != a, "duplicate gives a distinct handle");
    char buf[8];
    XIoStatusBlock iosb{};
    check(XF_NT_OK(xf_read(dup, buf, 4, nullptr, &iosb)), "the duplicate reads the same file");

    check(xf_close(dup) == XF_SUCCESS, "closed the duplicate");
    check(xf_close(dup) == XF_INVALID_HANDLE, "closing it twice is INVALID_HANDLE");
    check(xf_close(a) == XF_SUCCESS, "closed the original");
    check(xf_open_handle_count() == before, "no handle leaked");

    NTSTATUS s;
    open_at("\\??\\D:\\media\\intro.xmv", XF_GENERIC_READ, XF_FILE_OPEN, 0, &s);
    XfHandle bad = 0xFFFF;
    check(xf_close(bad) == XF_INVALID_HANDLE, "an out-of-range handle is rejected, not dereferenced");
}

static void test_attributes()
{
    printf("\nfile attributes\n");
    std::vector<char> buf;
    std::string p = "\\??\\D:\\media\\intro.xmv";
    buf.assign(p.begin(), p.end());
    XAnsiString name{(uint16_t)p.size(), (uint16_t)p.size(), buf.data()};
    XObjectAttributes oa{XF_INVALID_HANDLE_VALUE, &name, 0};

    XFileNetworkOpenInformation info{};
    NTSTATUS s = xf_query_full_attributes(&oa, &info);
    check(XF_NT_OK(s), "queried full attributes");
    check(info.EndOfFile.QuadPart == 14, "EndOfFile is the file size");
    check(info.AllocationSize.QuadPart == 4096, "AllocationSize is rounded up to a block");
    check((info.FileAttributes & 0x10) == 0, "a file is not marked as a directory");

    XFileFullDirectoryInformation dinfo{};
    std::string d = "\\??\\D:\\media";
    std::vector<char> dbuf(d.begin(), d.end());
    XAnsiString dname{(uint16_t)d.size(), (uint16_t)d.size(), dbuf.data()};
    XObjectAttributes doa{XF_INVALID_HANDLE_VALUE, &dname, 0};
    check(XF_NT_OK(xf_query_directory_attributes(&doa, &dinfo)), "queried directory attributes");
    check((dinfo.basic.FileAttributes & 0x10) != 0, "a directory is marked FILE_ATTRIBUTE_DIRECTORY");
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0); // so a hang shows how far it got
    printf("BLACK loader -- POSIX file layer tests\n");

    char tmpl[] = "/tmp/blackxfileXXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) { perror("mkdtemp"); return 1; }
    g_root = dir;
    g_game = g_root + "/game";
    g_hdd = g_root + "/hdd";

    mk("game/media/sub");
    put("game/media/intro.xmv", kIntro);
    put("game/media/credits.xmv", "CREDITS");
    put("game/default.xbe", "XBEHDR");

    xf_init(g_game, g_hdd);
    add_links();

    test_resolution_and_read();
    test_readonly_media();
    test_directory_enumeration();
    test_volume_information();
    test_raw_volume();
    test_handles();
    test_attributes();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);

    // Leave nothing behind: the layer may hold open descriptors into g_root.
    fflush(stdout);
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_root.c_str());
    if (system(cmd) != 0) printf("(note: could not remove %s)\n", g_root.c_str());
    return g_failures ? 1 : 0;
}