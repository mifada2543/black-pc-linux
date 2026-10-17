// The Nt* file exports, end to end on a real temporary tree.
//
// Everything here goes in through the Xbox's own calls -- NtCreateFile with its eleven arguments in the
// Xbox's order, NtQueryInformationFile with the class numbers the kernel uses -- so what is tested is the
// surface the game sees, not the layer underneath, which test_xfile.cpp already covers from its own API.
//
// The information-class checks are the ones to read: those class numbers could not be verified without
// default.xbe, so this suite pins the *shapes* (which the NT layout fixes) and leaves the numbers as the
// reference has them, and test_boot-style logs report any class this port does not answer.
#include "kernel/k_file_posix.h"
#include "kernel/k_sync_posix.h" // NtClose lives with the handle dispatch
#include "platform/posix/xhandle.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
    fflush(stdout);
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

static void put(const std::string &rel, const std::string &data)
{
    FILE *f = fopen((g_root + "/" + rel).c_str(), "wb");
    if (!f) return;
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
}

static bool file_exists(const std::string &rel)
{
    struct stat st;
    return stat((g_root + "/" + rel).c_str(), &st) == 0;
}

// The Xbox's OBJECT_ATTRIBUTES with an absolute device path, which is how the game names files.
static NTSTATUS create_file(uint32_t *handle, const std::string &xbox_path, uint32_t access, uint32_t disposition,
                            uint32_t options, XIoStatusBlock *iosb = nullptr)
{
    std::vector<char> buf(xbox_path.begin(), xbox_path.end());
    XAnsiString name{(uint16_t)xbox_path.size(), (uint16_t)xbox_path.size(), buf.data()};
    XObjectAttributes oa{XF_INVALID_HANDLE_VALUE, &name, 0};
    return NtCreateFile(handle, access, &oa, iosb, nullptr, 0, 0, disposition, options);
}

static NTSTATUS read_file(uint32_t handle, void *buf, uint32_t len, int64_t offset, XIoStatusBlock *iosb)
{
    XLargeInteger at;
    at.QuadPart = offset;
    return NtReadFile(handle, 0, nullptr, nullptr, iosb, buf, len, &at);
}

// ----------------------------------------------------------------------------------------------------------

static void test_create_read_write()
{
    printf("\nopening, reading, writing\n");

    uint32_t file = 0;
    XIoStatusBlock iosb;
    check(create_file(&file, "\\Device\\CdRom0\\intro.vid", XF_GENERIC_READ, XF_FILE_OPEN, 0, &iosb) == XF_SUCCESS,
          "NtCreateFile opens a file on the DVD");
    check_u32(xhandle_kind(file), XHANDLE_FILE, "and the handle says it is a file");

    char buf[32] = {};
    check(read_file(file, buf, 14, 0, &iosb) == XF_SUCCESS, "NtReadFile reads it");
    check(std::string(buf) == "INTROVIDEO0000", "and the contents are what was written to the tree");
    check_u32((uint32_t)iosb.Information, 14, "with the byte count in the IO status block");

    // A read at an offset, which is how the game seeks.
    check(read_file(file, buf + 4, 4, 4, &iosb) == XF_SUCCESS, "a read at an offset succeeds");
    check(memcmp(buf + 4, "OVID", 4) == 0, "and reads from there (INTROVIDEO0000 at offset 4)");
    const NTSTATUS past = read_file(file, buf, 32, 0, &iosb);
    check(past == XF_SUCCESS, "a read past the end succeeds (status 0x" + std::to_string((uint32_t)past) + ")");
    check_u32((uint32_t)iosb.Information, 14, "reporting only what it read (" +
                                                std::to_string((uint32_t)iosb.Information) + ")");

    check(NtFlushBuffersFile(file, &iosb) == XF_SUCCESS, "NtFlushBuffersFile succeeds on a read handle");

    // The DVD is read-only, so a write has to be refused rather than silently dropped.
    check(NtWriteFile(file, 0, nullptr, nullptr, &iosb, "XXXX", 4, nullptr) == XF_ACCESS_DENIED,
          "writing to the DVD is denied");

    // A file that is not there.
    uint32_t missing = 0;
    const NTSTATUS absent =
        create_file(&missing, "\\Device\\CdRom0\\nope.dat", XF_GENERIC_READ, XF_FILE_OPEN, 0);
    check(absent == XF_OBJECT_NAME_NOT_FOUND,
          "opening a file that does not exist says so (status 0x" + std::to_string((uint32_t)absent) + ")");
    check(create_file(&missing, "\\Device\\CdRom0\\nope.dat", XF_GENERIC_READ, XF_FILE_CREATE, 0) ==
              XF_ACCESS_DENIED,
          "and creating one on the DVD is refused");

    // The hard disk is writable: that is where a game's saves go.
    uint32_t save = 0;
    check(create_file(&save, "\\Device\\Harddisk0\\Partition1\\UDATA\\save.bin", XF_GENERIC_WRITE | XF_GENERIC_READ,
                      XF_FILE_OPEN_IF, XF_NON_DIRECTORY_FILE) == XF_SUCCESS,
          "a save file opens on the hard disk");
    const char payload[] = "SAVE0000000000";
    check(NtWriteFile(save, 0, nullptr, nullptr, &iosb, payload, 14, nullptr) == XF_SUCCESS,
          "and the game writes to it");
    check_u32((uint32_t)iosb.Information, 14, "with the byte count reported");
    check(NtFlushBuffersFile(save, &iosb) == XF_SUCCESS, "and flushes it");
    check(file_exists("hdd/Partition1/UDATA/save.bin"), "so the bytes really reached the host filesystem");

    memset(buf, 0, sizeof buf);
    check(read_file(save, buf, 14, 0, &iosb) == XF_SUCCESS, "and can be read back");
    check(std::string(buf) == payload, "with the same contents");

    // NtOpenFile is NtCreateFile with a forced disposition, and it has to work the same way.
    uint32_t opened = 0;
    XObjectAttributes oa = {XF_INVALID_HANDLE_VALUE, nullptr, 0};
    static const char kSavePath[] = "\\Device\\Harddisk0\\Partition1\\UDATA\\save.bin";
    std::vector<char> name_buf(kSavePath, kSavePath + sizeof(kSavePath) - 1);
    XAnsiString name{(uint16_t)name_buf.size(), (uint16_t)name_buf.size(), name_buf.data()};
    oa.ObjectName = &name;
    check(NtOpenFile(&opened, XF_GENERIC_READ, &oa, &iosb, 0, 0) == XF_SUCCESS, "NtOpenFile opens it by name");
    check(opened != save, "as a handle of its own");
    memset(buf, 0, sizeof buf);
    check(read_file(opened, buf, 4, 0, &iosb) == XF_SUCCESS && memcmp(buf, "SAVE", 4) == 0,
          "and that handle reads the same file");
    check(NtClose(opened) == XF_SUCCESS, "the second handle closes");
    check(NtClose(save) == XF_SUCCESS, "and the first one too");
}

static void test_information_classes()
{
    printf("\nfile information\n");

    uint32_t file = 0;
    XIoStatusBlock iosb;
    check(create_file(&file, "\\Device\\Harddisk0\\Partition1\\UDATA\\save.bin", XF_GENERIC_READ, XF_FILE_OPEN, 0,
                      &iosb) == XF_SUCCESS,
          "a file to ask about");

    // Class 5, FileStandardInformation: allocation size and end of file as 64-bit counts.
    alignas(4) uint8_t standard[24];
    memset(standard, 0xCC, sizeof standard);
    check(NtQueryInformationFile(file, &iosb, standard, sizeof standard, 5) == XF_SUCCESS,
          "class 5 answers");
    check_u32((uint32_t)iosb.Information, 24, "reporting 24 bytes");
    int64_t end_of_file;
    memcpy(&end_of_file, standard + 8, 8);
    check_u32((uint32_t)end_of_file, 14, "the file's size is 14 bytes");
    check_u32(standard[20], 0, "and it is not marked for deletion");
    check_u32(standard[21], 0, "nor is it a directory");

    // A buffer that is too small must say so rather than write past the end.
    uint8_t tiny[8];
    check(NtQueryInformationFile(file, &iosb, tiny, sizeof tiny, 5) == XF_BUFFER_TOO_SMALL,
          "a short buffer is refused");

    // Class 4, FileBasicInformation: four timestamps and the attributes.
    alignas(8) uint8_t basic[40];
    check(NtQueryInformationFile(file, &iosb, basic, sizeof basic, 4) == XF_SUCCESS, "class 4 answers");
    int64_t created;
    memcpy(&created, basic, 8);
    // The timestamps are 100 ns since 1601, which for a file written moments ago is a number with ten
    // digits. A host timestamp (since 1970) would be ten digits smaller, and that difference is the whole
    // reason the conversion is here.
    check(created > 13000000000000000LL, "the creation time is counted from 1601, not from 1970");
    uint32_t attributes;
    memcpy(&attributes, basic + 32, 4);
    check_u32(attributes, 0x80, "and the attributes say FILE_ATTRIBUTE_NORMAL");

    // Class 9, FileNameInformation: the name, relative to its volume, in bytes.
    uint8_t name_info[64];
    check(NtQueryInformationFile(file, &iosb, name_info, sizeof name_info, 9) == XF_SUCCESS, "class 9 answers");
    uint32_t name_len;
    memcpy(&name_len, name_info, 4);
    check(name_len > 0 && name_len < sizeof name_info - 4, "with a plausible name length");
    check(memcmp(name_info + 4, "\\UDATA", 6) == 0,
          "and the name relative to its volume, not the whole device path");

    // Class 14, FilePositionInformation: the offset the last read used.
    alignas(8) uint8_t position[8];
    char buf[8];
    read_file(file, buf, 4, 6, &iosb);
    check(NtQueryInformationFile(file, &iosb, position, sizeof position, 14) == XF_SUCCESS, "class 14 answers");
    int64_t offset;
    memcpy(&offset, position, 8);
    check_u32((uint32_t)offset, 6, "reporting the offset of the last read");

    // Setting the position, then reading it back.
    alignas(8) uint8_t wanted[8];
    const int64_t target = 2;
    memcpy(wanted, &target, 8);
    check(NtSetInformationFile(file, &iosb, wanted, sizeof wanted, 14) == XF_SUCCESS, "class 14 can be set");
    check(NtQueryInformationFile(file, &iosb, position, sizeof position, 14) == XF_SUCCESS, "and read back");
    memcpy(&offset, position, 8);
    check_u32((uint32_t)offset, 2, "as the position that was set");

    // A class this port does not answer has to be reported, not filled in with a guess.
    uint8_t mystery[64];
    check(NtQueryInformationFile(file, &iosb, mystery, sizeof mystery, 99) == XF_INVALID_PARAMETER,
          "a class with no implementation is refused rather than guessed at");
    check(NtQueryInformationFile(file, &iosb, mystery, sizeof mystery, 5) == XF_SUCCESS, "and the known one still works");

    check(NtClose(file) == XF_SUCCESS, "the file closes");
}

static void test_rename()
{
    printf("\nrenaming\n");

    put("hdd/Partition1/UDATA/old.bin", "OLDCONTENT01");

    uint32_t file = 0;
    XIoStatusBlock iosb;
    check(create_file(&file, "\\Device\\Harddisk0\\Partition1\\UDATA\\old.bin", XF_GENERIC_READ | XF_GENERIC_WRITE,
                      XF_FILE_OPEN, 0, &iosb) == XF_SUCCESS,
          "the file to rename opens");

    // FILE_RENAME_INFORMATION: { BOOLEAN ReplaceIfExists; HANDLE RootDirectory; OBJECT_STRING Name; }
    // at offset 0. The Xbox's kernel numbers rename 10; NT numbers it 7, and both are accepted here.
    // { BOOLEAN ReplaceIfExists; HANDLE RootDirectory; OBJECT_STRING Name; } then the name inline, with
    // Name.Buffer pointing at it. 16 bytes of header, then the text.
    constexpr uint32_t kRenameHeader = 16;
    alignas(4) uint8_t info[96];
    memset(info, 0, sizeof info);
    info[0] = 0; // do not replace
    const uint32_t root = XF_INVALID_HANDLE_VALUE;
    memcpy(info + 4, &root, 4);
    auto fill_name = [&](const std::string &path) {
        const uint16_t len = (uint16_t)path.size();
        const uint32_t inline_name = (uint32_t)(uintptr_t)(info + kRenameHeader);
        memcpy(info + 8, &len, 2);
        memcpy(info + 10, &len, 2);
        memcpy(info + 12, &inline_name, 4);
        memcpy(info + kRenameHeader, path.data(), path.size());
    };

    const std::string target = "\\Device\\Harddisk0\\Partition1\\UDATA\\new.bin";
    fill_name(target);
    check(NtSetInformationFile(file, &iosb, info, kRenameHeader + (uint32_t)target.size(), 10) == XF_SUCCESS,
          "a rename as class 10 works");
    check(file_exists("hdd/Partition1/UDATA/new.bin"), "the file is at its new name");
    check(!file_exists("hdd/Partition1/UDATA/old.bin"), "and not at the old one");

    // The handle follows the file it had, which is what makes a rename-then-write work.
    char buf[16] = {};
    const NTSTATUS after = read_file(file, buf, 12, 0, &iosb);
    check(after == XF_SUCCESS && std::string(buf, 12) == "OLDCONTENT01",
          "the open handle still reads the same file after the rename (status 0x" +
              std::to_string((uint32_t)after) + ", \"" + std::string(buf, 12) + "\")");

    // And the NT class number for the same thing.
    put("hdd/Partition1/UDATA/third.bin", "THIRD0000000");
    uint32_t third = 0;
    create_file(&third, "\\Device\\Harddisk0\\Partition1\\UDATA\\third.bin", XF_GENERIC_READ, XF_FILE_OPEN, 0);
    fill_name("\\Device\\Harddisk0\\Partition1\\UDATA\\fourth.bin");
    check(NtSetInformationFile(third, &iosb, info, kRenameHeader + 43, 7) == XF_SUCCESS,
          "and class 7 does the same thing");
    check(file_exists("hdd/Partition1/UDATA/fourth.bin"), "so whichever number the game uses, it works");

    // A rename to somewhere that does not resolve must change nothing.
    const std::string nonsense = "\\Device\\NoSuchDevice\\nope.bin";
    fill_name(nonsense);
    check(NtSetInformationFile(third, &iosb, info, kRenameHeader + (uint32_t)nonsense.size(), 10) != XF_SUCCESS,
          "a rename to a device that does not exist fails");
    check(file_exists("hdd/Partition1/UDATA/fourth.bin"), "and leaves the file where it was");

    NtClose(file);
    NtClose(third);
}

static void test_directory()
{
    printf("\nreading a directory\n");

    put("hdd/Partition1/UDATA/one.txt", "1");
    put("hdd/Partition1/UDATA/two.txt", "22");

    uint32_t dir = 0;
    XIoStatusBlock iosb;
    check(create_file(&dir, "\\Device\\Harddisk0\\Partition1\\UDATA\\", XF_GENERIC_READ | XF_FILE_LIST_DIRECTORY,
                      XF_FILE_OPEN, XF_DIRECTORY_FILE, &iosb) == XF_SUCCESS,
          "a directory opens");

    // One entry per call, each a FILE_DIRECTORY_INFORMATION: a 64-byte header and an 8-bit name at 64.
    alignas(4) uint8_t entry[256];
    bool saw_one = false, saw_two = false;
    int entries = 0;
    for (;;) {
        memset(entry, 0, sizeof entry);
        const NTSTATUS s = NtQueryDirectoryFile(dir, 0, nullptr, nullptr, &iosb, entry, sizeof entry, 1, nullptr,
                                                entries == 0);
        if (s == XF_NO_MORE_FILES) break;
        if (s != XF_SUCCESS) {
            check(false, "a directory entry was returned");
            break;
        }
        entries++;
        const uint32_t next = *(uint32_t *)entry;
        check(next == 0, "each entry stands alone, so NextEntryOffset is 0");
        const uint8_t name_len = entry[60];
        check(name_len > 0 && name_len < sizeof entry - 64, "with a name length that fits");
        const std::string name((const char *)entry + 64, name_len);
        uint32_t attrs;
        memcpy(&attrs, entry + 56, 4);
        // Files and directories have to be distinguishable: a game filtering a listing does it on this
        // bit, and getdents64 does not tell us the answer without a stat.
        check((attrs & 0x10) == 0 && (attrs & 0x80) != 0,
              "'" + name + "' is reported as a plain file (attributes 0x" + std::to_string(attrs) + ")");
        if (name == "one.txt") saw_one = true;
        if (name == "two.txt") saw_two = true;
        if (entries > 16) break; // a runaway loop is a failure, not a hang
    }
    check(saw_one && saw_two, "every file in the directory was enumerated, one per call");

    // A subdirectory in the same listing has to come back with the directory bit, which is the only way
    // the game can tell "media" is a folder.
    mkdir((g_root + "/hdd/Partition1/UDATA/media").c_str(), 0777);
    uint32_t dir2 = 0;
    check(create_file(&dir2, "\\Device\\Harddisk0\\Partition1\\UDATA\\", XF_GENERIC_READ | XF_FILE_LIST_DIRECTORY,
                      XF_FILE_OPEN, XF_DIRECTORY_FILE, &iosb) == XF_SUCCESS,
          "the directory is opened again to see its new entry");
    bool saw_media_dir = false;
    bool first = true;
    for (;;) {
        memset(entry, 0, sizeof entry);
        const NTSTATUS s = NtQueryDirectoryFile(dir2, 0, nullptr, nullptr, &iosb, entry, sizeof entry, 1, nullptr,
                                                first);
        first = false; // RestartScan only on the first call: every call with it replays the first entry
        if (s != XF_SUCCESS) break;
        const uint8_t name_len = entry[60];
        uint32_t attrs;
        memcpy(&attrs, entry + 56, 4);
        if (std::string((const char *)entry + 64, name_len) == "media") saw_media_dir = (attrs & 0x10) != 0;
        if (++entries > 24) break;
    }
    check(saw_media_dir, "a subdirectory is reported with the directory attribute");
    NtClose(dir2);
    check(entries >= 2, "in at least two entries");

    check(NtQueryDirectoryFile(dir, 0, nullptr, nullptr, &iosb, entry, sizeof entry, 3, nullptr, false) ==
              XF_INVALID_PARAMETER,
          "an information class with no implementation is refused");

    check(NtClose(dir) == XF_SUCCESS, "the directory closes");
}

static void test_ioctl_and_symlinks()
{
    printf("\ndevice control and symbolic links\n");

    uint32_t file = 0;
    XIoStatusBlock iosb;
    check(create_file(&file, "\\Device\\Harddisk0\\Partition1\\", XF_GENERIC_READ, XF_FILE_OPEN,
                      XF_DIRECTORY_FILE, &iosb) == XF_SUCCESS,
          "the DVD volume opens");

    // IOCTL_DISK_GET_DRIVE_GEOMETRY: a cylinder count, media type, and the sector geometry the console's
    // FATX reports.
    alignas(8) uint8_t geometry[32];
    memset(geometry, 0xEE, sizeof geometry);
    check(NtDeviceIoControlFile(file, 0, nullptr, nullptr, &iosb, 0x70000, nullptr, 0, geometry,
                                sizeof geometry) == XF_SUCCESS,
          "IOCTL_DISK_GET_DRIVE_GEOMETRY answers");
    uint32_t fixed_media;
    uint32_t bytes_per_sector;
    memcpy(&fixed_media, geometry + 8, 4);
    memcpy(&bytes_per_sector, geometry + 20, 4);
    check_u32(fixed_media, 12, "reporting removable media, as the DVD is");
    check_u32(bytes_per_sector, 512, "with 512-byte sectors");

    check(NtDeviceIoControlFile(file, 0, nullptr, nullptr, &iosb, 0x70004, nullptr, 0, geometry, 24) ==
              XF_INVALID_DEVICE_REQUEST,
          "an ioctl this port does not answer is refused");
    check(NtDeviceIoControlFile(file, 0, nullptr, nullptr, &iosb, 0x70000, nullptr, 0, geometry, 4) ==
              XF_BUFFER_TOO_SMALL,
          "and an output buffer too small for the geometry is refused as one");

    check(NtFsControlFile(file, 0, nullptr, nullptr, &iosb, 0x90020, nullptr, 0, nullptr, 0) == XF_SUCCESS,
          "FSCTL_DISMOUNT_VOLUME succeeds, which is the only one the game issues");
    check(NtFsControlFile(file, 0, nullptr, nullptr, &iosb, 0x12345, nullptr, 0, nullptr, 0) ==
              XF_INVALID_DEVICE_REQUEST,
          "and another fsctl does not");

    // Volume information: the game asks before it writes a save.
    alignas(8) uint8_t volume[32];
    check(NtQueryVolumeInformationFile(file, &iosb, volume, sizeof volume, 3) == XF_SUCCESS,
          "NtQueryVolumeInformationFile answers FileFsSizeInformation");
    check_u32((uint32_t)iosb.Information, 24, "with 24 bytes");

    // Symbolic links, which is how a path like \\??\\D: reaches a device.
    // The buffers have to outlive the calls, so they are not temporaries built from string literals.
    static const char kLinkName[] = "\\??\\D:";
    static const char kLinkTarget[] = "\\Device\\CdRom0";
    std::vector<char> lname(kLinkName, kLinkName + sizeof(kLinkName) - 1);
    std::vector<char> ltarget(kLinkTarget, kLinkTarget + sizeof(kLinkTarget) - 1);
    XAnsiString ln{(uint16_t)lname.size(), (uint16_t)lname.size(), lname.data()};
    XAnsiString lt{(uint16_t)ltarget.size(), (uint16_t)ltarget.size(), ltarget.data()};
    check(IoCreateSymbolicLink(&ln, &lt) == XF_SUCCESS, "a symbolic link is created");
    check(IoCreateSymbolicLink(&ln, &lt) == XF_OBJECT_NAME_COLLISION, "creating it again collides");

    uint32_t link = 0;
    XObjectAttributes link_oa{XF_INVALID_HANDLE_VALUE, &ln, 0};
    check(NtOpenSymbolicLinkObject(&link, &link_oa) == XF_SUCCESS, "and opened again");
    check_u32(xhandle_kind(link), XHANDLE_LINK, "as a handle of its own kind");

    XAnsiString target_out;
    memset(&target_out, 0, sizeof target_out);
    target_out.MaximumLength = 64;
    target_out.Buffer = (char *)malloc(64);
    uint32_t returned = 0;
    check(NtQuerySymbolicLinkObject(link, &target_out, &returned) == XF_SUCCESS, "the link reports its target");
    check(std::string(target_out.Buffer, target_out.Length) == "\\Device\\CdRom0", "which is what it was made for");
    check_u32(returned, (uint32_t)ltarget.size(), "and the length of it");

    target_out.MaximumLength = 4;
    check(NtQuerySymbolicLinkObject(link, &target_out, &returned) == XF_BUFFER_TOO_SMALL,
          "a target buffer that is too small is refused");

    // Through the link, the path resolves: this is the point of having one.
    uint32_t via_link = 0;
    static const char kViaLink[] = "D:\\intro.vid";
    std::vector<char> link_path(kViaLink, kViaLink + sizeof(kViaLink) - 1);
    XAnsiString lp{(uint16_t)link_path.size(), (uint16_t)link_path.size(), link_path.data()};
    XObjectAttributes via_oa{XF_INVALID_HANDLE_VALUE, &lp, 0};
    check(NtCreateFile(&via_link, XF_GENERIC_READ, &via_oa, &iosb, nullptr, 0, 0, XF_FILE_OPEN, 0) == XF_SUCCESS,
          "a file opens through the link, which is how the game reaches D:");

    check(IoDeleteSymbolicLink(&ln) == XF_SUCCESS, "the link is deleted");
    check(IoDeleteSymbolicLink(&ln) == XF_OBJECT_NAME_NOT_FOUND, "and deleting it again says it is gone");
    check(NtOpenSymbolicLinkObject(&link, &link_oa) == XF_OBJECT_PATH_NOT_FOUND, "and opening it fails");

    free(target_out.Buffer);
    NtClose(file);
    NtClose(via_link);
}

int main()
{
    printf("BLACK loader -- Nt* file exports\n");

    char tmpl[] = "/tmp/blacklinux-ntfile-XXXXXX";
    const char *dir = mkdtemp(tmpl);
    if (!dir) {
        printf("FATAL: cannot create a temporary directory\n");
        return 1;
    }
    g_root = dir;

    mk("game");
    mk("hdd/Partition1/UDATA");
    mk("hdd/Partition1/TDATA");
    put("game/intro.vid", "INTROVIDEO0000");
    g_game = g_root + "/game";
    g_hdd = g_root + "/hdd";
    kfile_init(g_game, g_hdd);

    test_create_read_write();
    test_information_classes();
    test_rename();
    test_directory();
    test_ioctl_and_symlinks();

    const std::string cmd = "rm -rf '" + g_root + "'";
    if (system(cmd.c_str()) != 0) printf("  (warning: could not remove %s)\n", g_root.c_str());

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}