// Large-file support. This has to be decided before the first include, because it selects between
// the 32-bit and the 64-bit variants of open/pread/lseek. The emulated Xbox hard disk is up to 8 GB
// (kPartitions[0]) and the game reads and writes raw volumes at absolute sector offsets past 2 GB; with
// a 32-bit off_t every one of those offsets silently wraps. CMake also sets it for this target, but
// the file has to be correct when compiled on its own.
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#define XFILE_DEFINED_FILE_OFFSET_BITS 1
#endif

#include "platform/posix/xfile.h"

#include "platform/posix/log.h"

#include "platform/posix/xhandle.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace {

struct Mount {
    std::string device; // as Xbox code spells it
    std::string key;    // lowercase
    std::string host;   // absolute host folder, no trailing slash
    bool readonly;
    int partition; // HDD partition number (0 = whole disk), -1 for the DVD
};

struct PartitionGeom {
    uint64_t start, size;
};
// Partition layout of a retail 8 GB drive (0 = the whole disk). Verbatim from k_file.cpp: these
// numbers are what the game validates at boot, so they are data rather than a port decision.
const PartitionGeom kPartitions[] = {
    {0, 0x1DD156000ull},
    {0xABE80000ull, 0x1312D6000ull}, // 1: E
    {0x8CA80000ull, 0x1F400000ull},  // 2: C
    {0x00080000ull, 0x2EE00000ull},  // 3: X
    {0x2EE80000ull, 0x2EE00000ull},  // 4: Y
    {0x5DC80000ull, 0x2EE00000ull},  // 5: Z
};

constexpr size_t kMaxHandles = 512;
constexpr size_t kDirBufSize = 32 * 1024;       // one getdents64 block per directory handle
constexpr uint32_t kDirInfoHeaderSize = 64;     // fixed part of FILE_DIRECTORY_INFORMATION; name at 64
static_assert(offsetof(XFileDirectoryInformation, FileName) == kDirInfoHeaderSize,
              "FileName must sit at offset 64: the Xbox game code reads it there");

// The raw getdents64 record. Deliberately not glibc's `struct dirent`: with _FILE_OFFSET_BITS=64 its
// layout differs from what the syscall writes, and recomputing the record length from sizeof() then
// walks off the end of the buffer. The kernel always fills d_reclen, so just use it.
struct LinuxDirent64 {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
};

struct Handle {
    bool used = false;
    int fd = -1;
    std::string xbox_path;
    // The host path this handle resolved to. Kept because the directory listing has to stat each entry --
    // getdents64 does not say what kind of file it is -- and the only way back to the host is here.
    std::string host_path;
    const Mount *mount = nullptr;
    bool raw_volume = false;
    uint32_t access = 0;
    bool delete_pending = false;
    // A symbolic-link object has no file behind it: xbox_path is the link's own name and link_target is
    // what it resolves to, which is what NtQuerySymbolicLinkObject reports.
    bool is_link = false;
    std::string link_target;
    // The file position the Xbox kernel would keep per file object: the game reads it back with
    // FilePositionInformation, and there is no seek() here -- every read and write names its offset.
    int64_t last_offset = 0;
    // Directory cursor: a live getdents64 buffer plus how far we have consumed it. dir_len is the number
// of bytes the last read actually filled -- dir_buf stays at its full allocation, so dir_buf.size()
// must never be used as the valid length. Keeping the buffer per handle is what makes "one entry per
// call, resume where we left off" work.
    std::vector<char> dir_buf;
    size_t dir_pos = 0;
    size_t dir_len = 0;
    bool dir_eof = false;
    bool is_dir = false;
};

std::recursive_mutex g_mutex;
std::vector<Mount> g_mounts;
std::string g_hdd;
std::map<std::string, std::string> g_links; // lowercase name -> target
Handle g_handles[kMaxHandles];

std::string lower(std::string s)
{
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

struct Resolved {
    std::string xbox_path;    // canonical device path
    const Mount *mount = nullptr;
    std::string rest;         // part after the device, starts with '/' or is empty
    std::string host_path;    // absolute host path
    bool raw_volume = false;
};

void complete(XIoStatusBlock *iosb, NTSTATUS s, uint32_t info)
{
    if (!iosb) return;
    iosb->Status = s;
    iosb->Information = info;
}

uint32_t err_to_status(int e)
{
    switch (e) {
    case ENOENT: return XF_OBJECT_NAME_NOT_FOUND;
    case ENOTDIR: return XF_OBJECT_PATH_NOT_FOUND;
    case EACCES:
    case EPERM:
    case EROFS: return XF_ACCESS_DENIED;
    case ENOTEMPTY: return XF_OBJECT_NAME_COLLISION;
    case EISDIR:
    case EINVAL: return XF_INVALID_PARAMETER;
    case EMFILE:
    case ENFILE: return XF_NO_MEMORY;
    default: return XF_UNSUCCESSFUL;
    }
}

bool make_dirs(const std::string &dir)
{
    if (dir.empty()) return false;
    struct stat st;
    if (stat(dir.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    size_t slash = dir.find_last_of('/');
    if (slash != std::string::npos && slash > 0) make_dirs(dir.substr(0, slash));
    if (mkdir(dir.c_str(), 0777) == 0) return true;
    return errno == EEXIST;
}

// Canonical Xbox object path -> (mount, remainder, host path). Ported from k_file.cpp's resolve(),
// which is already portable C++; the two Win32 calls it used (GetFileAttributesW, GetFullPathNameW)
// are gone because the existence test is now the open() itself and the host path is already absolute.
NTSTATUS resolve(uint32_t root, const XAnsiString *name, Resolved &r)
{
    std::lock_guard<std::recursive_mutex> g(g_mutex);
    std::string path = name ? xstr(name) : std::string();
    if (root == 0xFFFFFFFD) { // ObDosDevicesDirectory(): XAPI passes "d:\x" relative to \??\ like this
        path = "\\??\\" + path;
        root = XF_INVALID_HANDLE_VALUE;
    }
    if (root != XF_INVALID_HANDLE_VALUE) {
        const uint32_t ri = xhandle_index(root);
        if (ri >= kMaxHandles || !g_handles[ri].used) return XF_INVALID_HANDLE;
        std::string base = g_handles[ri].xbox_path;
        if (!path.empty() && path[0] != '\\' && (base.empty() || base.back() != '\\')) base += '\\';
        path = base + path;
    }
    // Symlink expansion, bounded: a cycle must not hang the loader.
    for (int depth = 0; depth < 8; depth++) {
        std::string lp = lower(path);
        if (lp.rfind("\\??\\", 0) == 0) {
            path = path.substr(4);
            continue;
        }
        if (lp.rfind("\\dosdevices\\", 0) == 0) {
            path = path.substr(12);
            continue;
        }
        if (path.size() >= 2 && path[1] == ':' && isalpha((unsigned char)path[0])) {
            auto it = g_links.find("\\??\\" + lower(path.substr(0, 2)));
            if (it == g_links.end()) return XF_OBJECT_PATH_NOT_FOUND;
            path = it->second + path.substr(2);
            continue;
        }
        break;
    }
    std::string lp = lower(path);
    const Mount *best = nullptr;
    for (const auto &m : g_mounts) {
        // A device prefix matches on a path-component boundary, so \Device\Harddisk0\Partition1
        // never matches \Device\Harddisk0\Partition10.
        if (lp.rfind(m.key, 0) == 0 && (lp.size() == m.key.size() || lp[m.key.size()] == '\\'))
            if (!best || m.key.size() > best->key.size()) best = &m;
    }
    if (!best) return XF_OBJECT_PATH_NOT_FOUND;
    r.mount = best;
    r.rest = path.substr(best->key.size());
    r.raw_volume = r.rest.empty();
    r.xbox_path = best->device + r.rest;
    std::string rest = r.rest;
    while (!rest.empty() && (rest.back() == '\\' || rest.back() == '/')) rest.pop_back();
    for (char &c : rest)
        if (c == '\\') c = '/';
    r.host_path = best->host + rest;
    return XF_SUCCESS;
}

// The Xbox object namespace is case-insensitive; a Linux filesystem is not. The Windows port got
// this for free from NTFS. When a direct open misses, walk the host path component by component and
// match each one case-insensitively, so \Device\CdRom0\MEDIA\INTRO.XMV still finds media/intro.xmv.
// Returns an empty string when a component does not exist under any spelling.
std::string host_path_ci(const Mount &m, const std::string &rest)
{
    std::string acc = m.host;
    size_t i = 0;
    for (int guard = 0; guard < 64; guard++) {
        // The remainder arrives with Xbox separators; the host wants '/'.
        auto sep = [](char c) { return c == '\\' || c == '/'; };
        while (i < rest.size() && sep(rest[i])) i++;
        size_t j = i;
        while (j < rest.size() && !sep(rest[j])) j++;
        if (i >= j) return acc;
        std::string comp = rest.substr(i, j - i);
        std::string want = lower(comp), found;
        if (DIR *d = opendir(acc.c_str())) {
            while (struct dirent *de = readdir(d)) {
                if (lower(de->d_name) == want) {
                    found = de->d_name;
                    break;
                }
            }
            closedir(d);
        }
        if (found.empty()) return {};
        acc += "/" + found;
        i = j;
    }
    return {};
}

// The arena entry a handle refers to, or null. Every public entry point resolves the handle through
// this, so a handle from another layer cannot reach the file table by accident.
Handle *file_of(XfHandle h)
{
    const uint32_t i = xhandle_index(h);
    return i < kMaxHandles && g_handles[i].used ? &g_handles[i] : nullptr;
}

int alloc_handle()
{
    for (size_t i = 1; i < kMaxHandles; i++) // slot 0 is never a handle
        if (!g_handles[i].used) return (int)i;
    return -1;
}

// NtCreateFile's option/disposition fixups, kept in one place because k_file.cpp has the same block
// and the reasons are the same.
NTSTATUS open_common(const char *what, XfHandle *out, uint32_t access, XObjectAttributes *oa, XIoStatusBlock *iosb,
                     XLargeInteger *alloc, uint32_t attrs, uint32_t share, uint32_t disp, uint32_t opts)
{
    if (!oa || !out) {
        complete(iosb, XF_INVALID_PARAMETER, 0);
        return XF_INVALID_PARAMETER;
    }
    Resolved r;
    NTSTATUS s = resolve(oa->RootDirectory, oa->ObjectName, r);
    if (!XF_NT_OK(s)) {
        complete(iosb, s, 0);
        return s;
    }

    uint32_t a = access, d = disp, o = opts;
    bool want_dir = (o & XF_DIRECTORY_FILE) != 0;
    bool raw = r.raw_volume;

    if (raw && r.mount->partition >= 0) {
        // Raw hard disk device: Partition0 (whole disk) or a partition opened for formatting. Backed
        // by a host file that grows as sectors are written; reads past the end return zeroes.
        r.host_path = g_hdd + "/Partition" + std::to_string(r.mount->partition) + ".raw";
        a = XF_GENERIC_READ | XF_GENERIC_WRITE | XF_SYNCHRONIZE | XF_FILE_READ_ATTRIBUTES;
        o = XF_NON_DIRECTORY_FILE;
        d = XF_FILE_OPEN_IF;
    } else if (raw) {
        // The DVD device itself: open the game folder so volume queries and IOCTLs have a handle.
        a = XF_FILE_LIST_DIRECTORY | XF_FILE_READ_ATTRIBUTES;
        o = XF_DIRECTORY_FILE;
        d = XF_FILE_OPEN;
        want_dir = true;
    } else if (r.mount->readonly) {
        // Read-only media. The Xbox kernel answers ACCESS_DENIED for a write disposition; for a plain
        // open it silently downgrades the access to read, which is what the loader did.
        if (d != XF_FILE_OPEN && d != XF_FILE_OPEN_IF) {
            complete(iosb, XF_ACCESS_DENIED, 0);
            return XF_ACCESS_DENIED;
        }
        if (a & XF_GENERIC_ALL) a |= XF_GENERIC_READ;
        a &= ~(XF_GENERIC_WRITE | XF_FILE_WRITE_DATA | XF_FILE_APPEND_DATA | XF_FILE_WRITE_EA | XF_FILE_WRITE_ATTRIBUTES |
                XF_DELETE);
        d = XF_FILE_OPEN;
        o &= ~XF_DELETE_ON_CLOSE;
    }

    // POSIX has no share modes; the Xbox game only ever uses the permissive ones.
    (void)share;

    // Read-only vs read-write comes from the access mask, not from the disposition: the game opens
    // some files with FILE_OPEN_IF purely to read them, and asking for O_RDWR there would fail on
    // anything the host has not marked writable.
    bool want_write = (a & (XF_GENERIC_WRITE | XF_GENERIC_ALL | XF_FILE_WRITE_DATA | XF_FILE_APPEND_DATA | XF_FILE_WRITE_EA |
                            XF_FILE_WRITE_ATTRIBUTES | XF_DELETE)) != 0;
    int flags = want_write ? O_RDWR : O_RDONLY;
    switch (d) {
    case XF_FILE_OPEN: break;                                 // must exist
    case XF_FILE_OPEN_IF: flags |= O_CREAT; break;            // create if missing, else keep contents
    case XF_FILE_CREATE: flags |= O_CREAT | O_EXCL; break;    // must not exist
    case XF_FILE_SUPERSEDE: flags |= O_TRUNC; break;          // create or truncate
    case XF_FILE_OVERWRITE: flags |= O_TRUNC; break;          // must exist, truncate
    case XF_FILE_OVERWRITE_IF: flags |= O_CREAT | O_TRUNC; break;
    default:
        complete(iosb, XF_INVALID_PARAMETER, 0);
        return XF_INVALID_PARAMETER;
    }
    if (o & XF_NON_DIRECTORY_FILE) flags |= O_NOFOLLOW;
    // A write request against the DVD must fail even if the host folder happens to be writable:
    // the game's save paths go to partitions, and this keeps a read-only mount honest.
    if (r.mount->readonly && d != XF_FILE_OPEN && d != XF_FILE_OPEN_IF) flags |= O_RDONLY;

    int fd = open(r.host_path.c_str(), flags, 0666);
    if (fd < 0 && errno == ENOENT && !raw) {
        std::string ci = host_path_ci(*r.mount, r.rest);
        if (!ci.empty()) {
            r.host_path = ci;
            fd = open(r.host_path.c_str(), flags, 0666);
        }
    }
    if (fd < 0) {
        NTSTATUS st = err_to_status(errno);
        complete(iosb, st, 0);
        return st;
    }

    struct stat sb;
    bool is_dir = fstat(fd, &sb) == 0 && S_ISDIR(sb.st_mode);
    if (want_dir && !is_dir) {
        close(fd);
        complete(iosb, XF_NOT_A_DIRECTORY, 0);
        return XF_NOT_A_DIRECTORY;
    }

    int slot = alloc_handle();
    if (slot < 0) {
        close(fd);
        complete(iosb, XF_NO_MEMORY, 0);
        return XF_NO_MEMORY;
    }
    Handle &h = g_handles[slot];
    h = Handle();
    h.used = true;
    h.fd = fd;
    h.xbox_path = r.xbox_path;
    h.host_path = r.host_path;
    h.mount = r.mount;
    h.raw_volume = raw;
    h.access = a;
    h.is_dir = is_dir;
    if (is_dir) h.dir_buf.resize(kDirBufSize);
    if (raw && r.mount->partition >= 0 && alloc && alloc->QuadPart > 0) {
        if (ftruncate(fd, alloc->QuadPart) != 0) { /* best effort; the game only does this on format */ }
    }
    *out = xhandle_make(XHANDLE_FILE, (uint32_t)slot);
    complete(iosb, XF_SUCCESS, 0);
    (void)what;
    (void)attrs;
    return XF_SUCCESS;
}

// getdents64 -> one entry per call. The Xbox contract is one entry per query with NextEntryOffset
// zeroed, so the host's chained entries are unpacked here.
NTSTATUS dir_next(Handle &h, void *info, uint32_t len, XIoStatusBlock *iosb)
{
    if (!info || len < 64) {
        complete(iosb, XF_BUFFER_TOO_SMALL, 0);
        return XF_BUFFER_TOO_SMALL;
    }
    for (;;) {
        if (h.dir_pos >= h.dir_len) {
            if (h.dir_eof) {
                complete(iosb, XF_NO_MORE_FILES, 0);
                return XF_NO_MORE_FILES;
            }
            // No lseek here: the directory fd's own position is the cursor. Rewinding it would make
            // every buffer refill replay the directory from the first entry, and the listing would
            // cycle forever instead of ending with NO_MORE_FILES. Only RestartScan rewinds.
            ssize_t n = syscall(SYS_getdents64, h.fd, h.dir_buf.data(), h.dir_buf.size());
            if (n <= 0) {
                h.dir_eof = true;
                h.dir_len = 0;
                complete(iosb, XF_NO_MORE_FILES, 0);
                return XF_NO_MORE_FILES;
            }
            h.dir_len = (size_t)n;
            h.dir_pos = 0;
        }
        size_t before = h.dir_pos;
        while (h.dir_pos < h.dir_len) {
            const LinuxDirent64 *de = (const LinuxDirent64 *)(h.dir_buf.data() + h.dir_pos);
            if (de->d_reclen < sizeof(LinuxDirent64) + 2 || h.dir_pos + de->d_reclen > h.dir_len) break;
            h.dir_pos += de->d_reclen;

            const char *name = de->d_name;
            if (!strcmp(name, ".") || !strcmp(name, "..")) continue;

            auto *out = (XFileDirectoryInformation *)info;
            // The fixed part of FILE_DIRECTORY_INFORMATION is 64 bytes, not 60: FileName is a char[1]
            // after FileNameLength, so it lands on the next dword boundary. k_file.cpp reads the name
            // at offset 64 too ("WCHAR name at offset 64").
            memset(out, 0, kDirInfoHeaderSize);
            out->NextEntryOffset = 0; // one entry per call, as the game expects
            out->FileIndex = (uint32_t)de->d_ino;
            uint32_t nlen = (uint32_t)strlen(name);
            // 64-bit timestamps in NT ticks (100 ns since 1601); the game only reads EndOfFile and
            // the attributes here, so d_ino/d_type is enough for the rest.
            out->EndOfFile.QuadPart = de->d_off;
            out->AllocationSize.QuadPart = de->d_off;
            // Most Linux filesystems report d_type as DT_UNKNOWN, and the game filters directory listings
            // on FILE_ATTRIBUTE_DIRECTORY, so an unknown type has to be resolved with a stat.
            bool is_dir = de->d_type == DT_DIR;
            if (de->d_type == DT_UNKNOWN) {
                struct stat st;
                const std::string child = h.host_path + "/" + name;
                is_dir = stat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
            }
            out->FileAttributes = is_dir ? 0x10 /* FILE_ATTRIBUTE_DIRECTORY */ : 0x80 /* NORMAL */;
            out->FileNameLength = nlen;
            uint32_t room = len - kDirInfoHeaderSize;
            uint32_t copy = nlen < room ? nlen : room;
            // 8-bit name: non-ASCII bytes would not survive an Xbox OCHAR field, so they become '_'.
            for (uint32_t i = 0; i < copy; i++) out->FileName[i] = (unsigned char)name[i] < 128 ? name[i] : '_';
            if (nlen > room) {
                complete(iosb, XF_BUFFER_OVERFLOW, kDirInfoHeaderSize + copy);
                return XF_BUFFER_OVERFLOW;
            }
            complete(iosb, XF_SUCCESS, kDirInfoHeaderSize + copy);
            return XF_SUCCESS;
        }
        // The inner loop consumed nothing but dir_pos < dir_len, so this buffer cannot be walked
        // further. Bail rather than spin: a truncated final record must not hang the game.
        if (h.dir_pos == before) {
            h.dir_eof = true;
            h.dir_len = 0;
            complete(iosb, XF_NO_MORE_FILES, 0);
            return XF_NO_MORE_FILES;
        }
    }
}

void fill_times(const struct stat &sb, XLargeInteger *c, XLargeInteger *a, XLargeInteger *w, XLargeInteger *ch)
{
    auto to_nt = [](const struct timespec &t) { return (int64_t)t.tv_sec * 10000000 + t.tv_nsec / 100; };
    c->QuadPart = to_nt(sb.st_ctim);
    a->QuadPart = to_nt(sb.st_atim);
    w->QuadPart = to_nt(sb.st_mtim);
    ch->QuadPart = to_nt(sb.st_ctim);
}

} // namespace

// ---------------------------------------------------------------------------

std::string xstr(const XAnsiString *s)
{
    if (!s || !s->Buffer) return {};
    return std::string(s->Buffer, s->Length);
}

NTSTATUS xf_add_symlink(const std::string &name, const std::string &target)
{
    std::lock_guard<std::recursive_mutex> g(g_mutex);
    // Object-name collision, not an overwrite: two links with one name would make which device a path
    // resolves to depend on creation order, and the game does check this status.
    if (g_links.count(lower(name))) return XF_OBJECT_NAME_COLLISION;
    g_links[lower(name)] = target;
    return XF_SUCCESS;
}

NTSTATUS xf_remove_symlink(const std::string &name)
{
    std::lock_guard<std::recursive_mutex> g(g_mutex);
    return g_links.erase(lower(name)) ? XF_SUCCESS : XF_OBJECT_NAME_NOT_FOUND;
}

NTSTATUS xf_open_symlink(XfHandle *out, const XAnsiString *name)
{
    if (!out) return XF_INVALID_PARAMETER;
    const std::string key = lower(name ? xstr(name) : std::string());
    std::lock_guard<std::recursive_mutex> g(g_mutex);
    auto it = g_links.find(key);
    if (it == g_links.end()) return XF_OBJECT_PATH_NOT_FOUND;

    // A link handle carries the link's name and target in the loader, because the Xbox's does too: the
    // game asks the object for its target afterwards with NtQuerySymbolicLinkObject.
    const int slot = alloc_handle();
    if (slot < 0) return XF_NO_MEMORY;
    Handle &h = g_handles[slot];
    h = Handle();
    h.used = true;
    h.fd = -1;
    h.xbox_path = "\\??\\" + it->first;
    h.link_target = it->second;
    h.is_link = true;
    *out = xhandle_make(XHANDLE_LINK, (uint32_t)slot);
    return XF_SUCCESS;
}

NTSTATUS xf_query_symlink(XfHandle h, XAnsiString *target, uint32_t *returned_length)
{
    std::lock_guard<std::recursive_mutex> g(g_mutex);
    const Handle *t = file_of(h);
    if (!t || !t->is_link) return XF_INVALID_HANDLE;
    if (returned_length) *returned_length = (uint32_t)t->link_target.size();
    if (!target || !target->Buffer) return XF_INVALID_PARAMETER;
    if (target->MaximumLength < t->link_target.size()) return XF_BUFFER_TOO_SMALL;
    memcpy(target->Buffer, t->link_target.data(), t->link_target.size());
    target->Length = (uint16_t)t->link_target.size();
    return XF_SUCCESS;
}

void xf_init(const std::string &game_dir, const std::string &hdd_dir)
{
    std::lock_guard<std::recursive_mutex> g(g_mutex);
    g_mounts.clear();
    g_links.clear();
    g_hdd = hdd_dir;
    make_dirs(hdd_dir);
    g_mounts.push_back({"\\Device\\CdRom0", lower("\\Device\\CdRom0"), game_dir, true, -1});
    g_mounts.push_back({"\\Device\\Harddisk0\\Partition0", lower("\\Device\\Harddisk0\\Partition0"), hdd_dir, false, 0});
    for (int p = 1; p <= 5; p++) {
        std::string dev = "\\Device\\Harddisk0\\Partition" + std::to_string(p);
        std::string dir = hdd_dir + "/Partition" + std::to_string(p);
        make_dirs(dir);
        g_mounts.push_back({dev, lower(dev), dir, false, p});
    }
    make_dirs(hdd_dir + "/Partition1/TDATA");
    make_dirs(hdd_dir + "/Partition1/UDATA");
}

std::string xf_device_host_path(const char *xbox_device_path)
{
    if (!xbox_device_path) return std::string();
    std::lock_guard<std::recursive_mutex> g(g_mutex);
    const std::string want = lower(xbox_device_path);
    for (const Mount &m : g_mounts)
        if (m.key == want) return m.host;
    return std::string();
}

std::string xf_handle_desc(XfHandle h)
{
    std::lock_guard<std::recursive_mutex> g(g_mutex);
    const Handle *t = file_of(h);
    return t ? t->xbox_path : "?";
}

uint32_t xf_open_handle_count()
{
    std::lock_guard<std::recursive_mutex> g(g_mutex);
    uint32_t n = 0;
    for (auto &h : g_handles)
        if (h.used) n++;
    return n;
}

NTSTATUS xf_create_file(XfHandle *out, uint32_t access, XObjectAttributes *oa, XIoStatusBlock *iosb, XLargeInteger *alloc,
                        uint32_t attrs, uint32_t share, uint32_t disp, uint32_t opts)
{
    std::lock_guard<std::recursive_mutex> g(g_mutex);
    return open_common("NtCreateFile", out, access, oa, iosb, alloc, attrs, share, disp, opts);
}

NTSTATUS xf_open_file(XfHandle *out, uint32_t access, XObjectAttributes *oa, uint32_t share, uint32_t opts)
{
    std::lock_guard<std::recursive_mutex> g(g_mutex);
    return open_common("NtOpenFile", out, access, oa, nullptr, nullptr, 0, share, XF_FILE_OPEN, opts);
}

NTSTATUS xf_close(XfHandle h)
{
    std::lock_guard<std::recursive_mutex> g(g_mutex);
    Handle *tp = file_of(h);
    if (!tp) return XF_INVALID_HANDLE;
    Handle &t = *tp;
    if (t.fd >= 0) close(t.fd);
    if (t.delete_pending && !t.xbox_path.empty()) {
        // Delete by resolved Xbox path so the mount-relative name is rebuilt, not guessed.
        Resolved r;
        XAnsiString s{(uint16_t)t.xbox_path.size(), (uint16_t)t.xbox_path.size(), (char *)t.xbox_path.data()};
        if (XF_NT_OK(resolve(XF_INVALID_HANDLE_VALUE, &s, r))) unlink(r.host_path.c_str());
    }
    t = Handle();
    return XF_SUCCESS;
}

NTSTATUS xf_duplicate(XfHandle src, XfHandle *out, uint32_t options)
{
    std::lock_guard<std::recursive_mutex> g(g_mutex);
    const Handle *sp = file_of(src);
    if (!sp) return XF_INVALID_HANDLE;
    int slot = alloc_handle();
    if (slot < 0) return XF_NO_MEMORY;
    Handle &s = g_handles[xhandle_index(src)];
    Handle &d = g_handles[slot];
    d = s;
    d.used = true;
    d.dir_buf.clear();
    d.dir_pos = 0;
    d.dir_len = 0;
    d.dir_eof = false;
    if (s.fd >= 0) {
        d.fd = dup(s.fd);
        if (d.fd < 0) return XF_UNSUCCESSFUL;
    }
    *out = (XfHandle)slot;
    if (options & 1) return xf_close(src); // DUPLICATE_CLOSE_SOURCE
    return XF_SUCCESS;
}

NTSTATUS xf_read(XfHandle h, void *buf, uint32_t len, XLargeInteger *offset, XIoStatusBlock *iosb)
{
    std::lock_guard<std::recursive_mutex> g(g_mutex);
    Handle *hp = file_of(h);
    if (!hp) {
        complete(iosb, XF_INVALID_HANDLE, 0);
        return XF_INVALID_HANDLE;
    }
    Handle &t = *hp;
    uint8_t *p = (uint8_t *)buf;
    uint32_t done = 0;
    if (offset && offset->QuadPart >= 0) t.last_offset = offset->QuadPart;

    if (offset && offset->QuadPart >= 0) {
        done = (uint32_t)pread(t.fd, p, len, (off_t)offset->QuadPart);
    } else if (t.raw_volume) {
        // A raw volume is a sparse host file: reading past the end is not an error on the Xbox, the
        // sectors are simply zero. k_file.cpp does this too, and the game relies on it when it reads
        // a partition it has not written yet.
        uint32_t want = len;
        done = (uint32_t)pread(t.fd, p, len, 0);
        (void)want;
        if ((int32_t)done < 0) done = 0;
    } else {
        done = (uint32_t)read(t.fd, p, len);
    }

    if (t.raw_volume) {
        if ((int32_t)done < 0) done = 0;
        ssize_t eof = lseek(t.fd, 0, SEEK_END);
        if (eof > 0 && done < len) memset(p + done, 0, len - done);
    }
    if ((int32_t)done < 0) {
        NTSTATUS s = err_to_status(errno);
        complete(iosb, s, 0);
        return s;
    }
    // NT's rule, which the Xbox inherits: a read that gets *some* of what it asked for succeeds and reports
    // the count; STATUS_END_OF_FILE is for a read that starts at or past the end and returns nothing.
    // Returning it for a short read is a status the game reads as a failure -- 0xC0000011 has the high bit
    // set, so NT_SUCCESS() is false for it -- and a game looping on status would give up early.
    if (done == 0 && len > 0 && !t.raw_volume) {
        complete(iosb, XF_END_OF_FILE, 0);
        return XF_END_OF_FILE;
    }
    complete(iosb, XF_SUCCESS, done);
    return XF_SUCCESS;
}

NTSTATUS xf_write(XfHandle h, const void *buf, uint32_t len, XLargeInteger *offset, XIoStatusBlock *iosb)
{
    std::lock_guard<std::recursive_mutex> g(g_mutex);
    Handle *hp = file_of(h);
    if (!hp) {
        complete(iosb, XF_INVALID_HANDLE, 0);
        return XF_INVALID_HANDLE;
    }
    Handle &t = *hp;
    if (t.mount && t.mount->readonly) {
        complete(iosb, XF_ACCESS_DENIED, 0);
        return XF_ACCESS_DENIED;
    }
    if (offset && offset->QuadPart >= 0) t.last_offset = offset->QuadPart;
    ssize_t done = offset && offset->QuadPart >= 0 ? pwrite(t.fd, buf, len, (off_t)offset->QuadPart)
                                                    : write(t.fd, buf, len);
    if (done < 0) {
        NTSTATUS s = err_to_status(errno);
        complete(iosb, s, 0);
        return s;
    }
    complete(iosb, XF_SUCCESS, (uint32_t)done);
    return XF_SUCCESS;
}

NTSTATUS xf_flush(XfHandle h)
{
    std::lock_guard<std::recursive_mutex> g(g_mutex);
    Handle *hp = file_of(h);
    if (!hp) return XF_INVALID_HANDLE;
    if (hp->fd >= 0) fsync(hp->fd);
    return XF_SUCCESS;
}

NTSTATUS xf_set_information_file(XfHandle h, uint32_t, const std::string &new_name)
{
    std::lock_guard<std::recursive_mutex> g(g_mutex);
    Handle *hp = file_of(h);
    if (!hp) return XF_INVALID_HANDLE;
    Handle &t = *hp;
    if (new_name.empty()) return XF_INVALID_PARAMETER;
    Resolved r;
    XAnsiString s{(uint16_t)t.xbox_path.size(), (uint16_t)t.xbox_path.size(), (char *)t.xbox_path.data()};
    if (!XF_NT_OK(resolve(XF_INVALID_HANDLE_VALUE, &s, r))) return XF_OBJECT_PATH_NOT_FOUND;
    if (rename(r.host_path.c_str(), new_name.c_str()) != 0) return err_to_status(errno);
    // Re-resolve so the handle keeps pointing at the file it had.
    Resolved r2;
    std::string np = new_name;
    XAnsiString s2{(uint16_t)np.size(), (uint16_t)np.size(), (char *)np.data()};
    if (XF_NT_OK(resolve(XF_INVALID_HANDLE_VALUE, &s2, r2))) t.xbox_path = r2.xbox_path;
    return XF_SUCCESS;
}

// ---- information classes -----------------------------------------------------
//
// The Xbox's class numbers are the NT ones for most of these (4, 13, 14 in particular), but the reference
// numbers rename as 10 where NT numbers it 7. Both are accepted below, and a rename request that does not
// resolve fails without touching the file, so an ambiguous class number costs a status rather than data.
//
// Only the classes the game can actually need are answered: the sizes it stats, the name it prints, the
// position it seeks by, and the timestamps a save file carries. Anything else is reported, not guessed at.

NTSTATUS xf_query_information(XfHandle h, uint32_t class_, void *info, uint32_t len, XIoStatusBlock *iosb)
{
    std::lock_guard<std::recursive_mutex> g(g_mutex);
    Handle *hp = file_of(h);
    if (!hp) {
        complete(iosb, XF_INVALID_HANDLE, 0);
        return XF_INVALID_HANDLE;
    }
    Handle &t = *hp;
    if (!info) {
        complete(iosb, XF_INVALID_PARAMETER, 0);
        return XF_INVALID_PARAMETER;
    }

    struct stat st {};
    const bool have_stat = t.fd >= 0 && fstat(t.fd, &st) == 0;
    uint8_t *out = (uint8_t *)info;

    switch (class_) {
    case 4: { // FileBasicInformation: four timestamps and the attributes (40 bytes)
        if (len < 40) {
            complete(iosb, XF_BUFFER_TOO_SMALL, 0);
            return XF_BUFFER_TOO_SMALL;
        }
        memset(out, 0, 40);
        uint8_t *cursor = out; // walks the four timestamps; the attributes word is written from `out`
        auto put_time = [&](int64_t sec, int64_t nsec) {
            // Host times are seconds since 1970 in a long long; the Xbox wants 100 ns since 1601.
            const int64_t units = (sec + 11644473600LL) * 10000000LL + nsec / 100;
            memcpy(cursor, &units, 8);
            cursor += 8;
        };
        if (have_stat) {
            put_time(st.st_ctim.tv_sec, st.st_ctim.tv_nsec);
            put_time(st.st_atim.tv_sec, st.st_atim.tv_nsec);
            put_time(st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
            put_time(st.st_mtim.tv_sec, st.st_mtim.tv_nsec); // the console has no separate change time
        }
        const uint32_t attrs = (uint32_t)(t.is_dir ? 0x10 /* FILE_ATTRIBUTE_DIRECTORY */
                                                    : 0x80 /* FILE_ATTRIBUTE_NORMAL */);
        memcpy(out + 32, &attrs, 4);
        complete(iosb, XF_SUCCESS, 40);
        return XF_SUCCESS;
    }
    case 5: { // FileStandardInformation: allocation size, end of file, links, flags (24 bytes)
        if (len < 24) {
            complete(iosb, XF_BUFFER_TOO_SMALL, 0);
            return XF_BUFFER_TOO_SMALL;
        }
        memset(out, 0, 24);
        const int64_t size = have_stat ? (int64_t)st.st_size : 0;
        // The console's FATX reports allocation size and end of file as the same rounded-up length; a
        // game that reads both wants them to agree.
        const int64_t alloc = (size + 511) & ~(int64_t)511;
        memcpy(out + 0, &alloc, 8);
        memcpy(out + 8, &size, 8);
        const uint32_t links = have_stat ? (uint32_t)st.st_nlink : 1;
        memcpy(out + 16, &links, 4);
        out[20] = t.delete_pending ? 1 : 0;
        out[21] = t.is_dir ? 1 : 0;
        complete(iosb, XF_SUCCESS, 24);
        return XF_SUCCESS;
    }
    case 9: { // FileNameInformation: a 4-byte length and the name, relative to its volume, in bytes
        std::string rel = t.xbox_path;
        const size_t dev_len = t.mount ? t.mount->device.size() : 0;
        if (dev_len && rel.size() > dev_len) rel = rel.substr(dev_len);
        if (rel.empty()) rel = "\\";
        if (len < 4) {
            complete(iosb, XF_BUFFER_TOO_SMALL, 0);
            return XF_BUFFER_TOO_SMALL;
        }
        const uint32_t n = (uint32_t)rel.size();
        memcpy(out, &n, 4);
        const uint32_t copy = n < len - 4 ? n : len - 4;
        memcpy(out + 4, rel.data(), copy);
        const NTSTATUS rs = copy < n ? XF_BUFFER_OVERFLOW : XF_SUCCESS;
        complete(iosb, rs, 4 + copy);
        return rs;
    }
    case 14: { // FilePositionInformation: the position the last read or write used (8 bytes)
        if (len < 8) {
            complete(iosb, XF_BUFFER_TOO_SMALL, 0);
            return XF_BUFFER_TOO_SMALL;
        }
        memcpy(out, &t.last_offset, 8);
        complete(iosb, XF_SUCCESS, 8);
        return XF_SUCCESS;
    }
    default:
        logf("file: NtQueryInformationFile('%s', class %u) not answered here", t.xbox_path.c_str(), class_);
        complete(iosb, XF_INVALID_PARAMETER, 0);
        return XF_INVALID_PARAMETER;
    }
}

NTSTATUS xf_set_information(XfHandle h, uint32_t class_, const void *info, uint32_t len, XIoStatusBlock *iosb)
{
    std::lock_guard<std::recursive_mutex> g(g_mutex);
    Handle *hp = file_of(h);
    if (!hp) {
        complete(iosb, XF_INVALID_HANDLE, 0);
        return XF_INVALID_HANDLE;
    }
    Handle &t = *hp;
    if (!info) {
        complete(iosb, XF_INVALID_PARAMETER, 0);
        return XF_INVALID_PARAMETER;
    }
    const uint8_t *p = (const uint8_t *)info;

    switch (class_) {
    case 7:   // FileRenameInformation, NT's number
    case 10: { // ... and the number the reference uses for it
        // { BOOLEAN ReplaceIfExists; HANDLE RootDirectory; OBJECT_STRING Name; }
        if (len < 12) {
            complete(iosb, XF_INVALID_PARAMETER, 0);
            return XF_INVALID_PARAMETER;
        }
        const XAnsiString *name = (const XAnsiString *)(p + 8);
        const uint32_t root = *(const uint32_t *)(p + 4);
        Resolved r;
        const NTSTATUS rs = resolve(root, name, r);
        if (!XF_NT_OK(rs)) {
            // A rename that does not resolve changes nothing: that is what makes accepting two possible
            // class numbers safe.
            complete(iosb, rs, 0);
            return rs;
        }
        // The source is this handle's own path, resolved again because a rename can cross volumes and the
        // host name has to be the one the handle had, not one guessed from the target.
        XAnsiString self{(uint16_t)t.xbox_path.size(), (uint16_t)t.xbox_path.size(), (char *)t.xbox_path.data()};
        Resolved mine;
        if (!XF_NT_OK(resolve(XF_INVALID_HANDLE_VALUE, &self, mine))) {
            complete(iosb, XF_OBJECT_PATH_NOT_FOUND, 0);
            return XF_OBJECT_PATH_NOT_FOUND;
        }
        const bool replace = p[0] != 0;
        if (rename(mine.host_path.c_str(), r.host_path.c_str()) != 0) {
            if (!(replace && errno == EEXIST)) {
                const NTSTATUS st = err_to_status(errno);
                complete(iosb, st, 0);
                return st;
            }
            unlink(r.host_path.c_str());
            if (rename(mine.host_path.c_str(), r.host_path.c_str()) != 0) {
                const NTSTATUS st = err_to_status(errno);
                complete(iosb, st, 0);
                return st;
            }
        }
        t.xbox_path = r.xbox_path;
        t.mount = r.mount;
        logf("file: renamed '%s' -> '%s'", mine.xbox_path.c_str(), r.xbox_path.c_str());
        complete(iosb, XF_SUCCESS, 0);
        return XF_SUCCESS;
    }
    case 14: { // FilePositionInformation: remember it for the next query
        if (len < 8) {
            complete(iosb, XF_INVALID_PARAMETER, 0);
            return XF_INVALID_PARAMETER;
        }
        int64_t pos;
        memcpy(&pos, p, 8);
        t.last_offset = pos;
        complete(iosb, XF_SUCCESS, 0);
        return XF_SUCCESS;
    }
    default:
        logf("file: NtSetInformationFile('%s', class %u) not answered here", t.xbox_path.c_str(), class_);
        complete(iosb, XF_INVALID_PARAMETER, 0);
        return XF_INVALID_PARAMETER;
    }
}

NTSTATUS xf_delete(XfHandle h)
{
    std::lock_guard<std::recursive_mutex> g(g_mutex);
    Handle *hp = file_of(h);
    if (!hp) return XF_INVALID_HANDLE;
    hp->delete_pending = true;
    return XF_SUCCESS; // performed at close, matching FILE_FLAG_DELETE_ON_CLOSE
}

NTSTATUS xf_query_volume(XfHandle h, void *info, uint32_t len, uint32_t class_, XIoStatusBlock *iosb)
{
    std::lock_guard<std::recursive_mutex> g(g_mutex);
    uint8_t *p = (uint8_t *)info;
    int part = 0;
    if (const Handle *hp = file_of(h))
        if (hp->mount) part = hp->mount->partition;

    // class 3 = FileFsSizeInformation (24 bytes), class 7 = FileFsFullSizeInformation (32).
    if ((class_ == 3 || class_ == 7) && part >= 1 && part <= 5) {
        // FATX-shaped numbers: 16 KB clusters, capped at the partition size, as k_file.cpp synthesises.
        uint64_t total = kPartitions[part].size;
        struct statvfs vfs {};
        uint64_t avail = 0;
        if (const Handle *hv = file_of(h); hv && hv->fd >= 0 && fstatvfs(hv->fd, &vfs) == 0)
            avail = (uint64_t)vfs.f_bavail * vfs.f_frsize;
        if (avail > total) avail = total;
        uint32_t need = class_ == 3 ? 24 : 32;
        if (len < need) {
            complete(iosb, XF_BUFFER_TOO_SMALL, 0);
            return XF_BUFFER_TOO_SMALL;
        }
        memset(p, 0, need);
        *(uint64_t *)(p + 0) = total / 0x4000;
        *(uint64_t *)(p + 8) = avail / 0x4000;
        if (class_ == 7) *(uint64_t *)(p + 16) = avail / 0x4000;
        uint32_t o = class_ == 3 ? 16 : 24;
        *(uint32_t *)(p + o) = 32;      // SectorsPerAllocationUnit
        *(uint32_t *)(p + o + 4) = 512; // BytesPerSector
        complete(iosb, XF_SUCCESS, need);
        return XF_SUCCESS;
    }
    switch (class_) {
    case XF_FileFsVolumeInformation: { // 1
        if (len < 18) {
            complete(iosb, XF_BUFFER_TOO_SMALL, 0);
            return XF_BUFFER_TOO_SMALL;
        }
        memset(p, 0, 18);
        *(uint32_t *)(p + 8) = 0x0B1AC000u + (uint32_t)part;
        complete(iosb, XF_SUCCESS, 18);
        return XF_SUCCESS;
    }
    case XF_FileFsAttributeInformation: { // 5
        const char *fs = part >= 0 ? "FATX" : "UDF";
        uint32_t n = (uint32_t)strlen(fs);
        if (len < 12 + n) {
            complete(iosb, XF_BUFFER_TOO_SMALL, 0);
            return XF_BUFFER_TOO_SMALL;
        }
        *(uint32_t *)(p + 0) = 0;
        *(uint32_t *)(p + 4) = 42; // MaxNameLength: the Xbox file system limit the game is told about
        *(uint32_t *)(p + 8) = n;
        memcpy(p + 12, fs, n);
        complete(iosb, XF_SUCCESS, 12 + n);
        return XF_SUCCESS;
    }
    default:
        complete(iosb, XF_INVALID_PARAMETER, 0);
        return XF_INVALID_PARAMETER;
    }
}

NTSTATUS xf_query_directory(XfHandle h, void *info, uint32_t len, const std::string &mask, bool restart, XIoStatusBlock *iosb)
{
    std::lock_guard<std::recursive_mutex> g(g_mutex);
    Handle *hp = file_of(h);
    if (!hp) {
        complete(iosb, XF_INVALID_HANDLE, 0);
        return XF_INVALID_HANDLE;
    }
    Handle &t = *hp;
    if (restart) {
        t.dir_pos = 0;
        t.dir_len = 0;
        t.dir_eof = false;
        if (t.is_dir && t.fd >= 0) {
            lseek(t.fd, 0, SEEK_SET); // the only place the directory cursor is rewound
            t.dir_buf.resize(kDirBufSize);
        }
    }
    // A mask is a DOS-style wildcard ("*.*", "*.bin"). The Xbox passes "*.*" for a full listing;
    // anything else we treat as "everything", which matches how k_file.cpp behaved for the patterns
    // it saw in practice (it forwarded the mask to the host unchanged).
    (void)mask;
    return dir_next(t, info, len, iosb);
}

NTSTATUS xf_query_full_attributes(XObjectAttributes *oa, XFileNetworkOpenInformation *out)
{
    std::lock_guard<std::recursive_mutex> g(g_mutex);
    if (!oa || !out) return XF_INVALID_PARAMETER;
    Resolved r;
    NTSTATUS s = resolve(oa->RootDirectory, oa->ObjectName, r);
    if (!XF_NT_OK(s)) return s;
    struct stat sb;
    if (stat(r.host_path.c_str(), &sb) != 0) return err_to_status(errno);
    memset(out, 0, sizeof *out);
    fill_times(sb, &out->CreationTime, &out->LastAccessTime, &out->LastWriteTime, &out->ChangeTime);
    out->EndOfFile.QuadPart = sb.st_size;
    out->AllocationSize.QuadPart = (sb.st_size + 4095) & ~(int64_t)4095;
    out->FileAttributes = S_ISDIR(sb.st_mode) ? 0x10 : 0x80;
    return XF_SUCCESS;
}

NTSTATUS xf_query_directory_attributes(XObjectAttributes *oa, XFileFullDirectoryInformation *out)
{
    std::lock_guard<std::recursive_mutex> g(g_mutex);
    if (!oa || !out) return XF_INVALID_PARAMETER;
    Resolved r;
    NTSTATUS s = resolve(oa->RootDirectory, oa->ObjectName, r);
    if (!XF_NT_OK(s)) return s;
    struct stat sb;
    if (stat(r.host_path.c_str(), &sb) != 0) return err_to_status(errno);
    memset(out, 0, sizeof *out);
    fill_times(sb, &out->basic.CreationTime, &out->basic.LastAccessTime, &out->basic.LastWriteTime, &out->basic.ChangeTime);
    out->basic.EndOfFile.QuadPart = sb.st_size;
    out->basic.AllocationSize.QuadPart = (sb.st_size + 4095) & ~(int64_t)4095;
    out->basic.FileAttributes = S_ISDIR(sb.st_mode) ? 0x10 : 0x80;
    return XF_SUCCESS;
}

NTSTATUS xf_device_io_control(XfHandle h, uint32_t code, void *out, uint32_t out_len, XIoStatusBlock *iosb)
{
    std::lock_guard<std::recursive_mutex> g(g_mutex);
    Handle *hp = file_of(h);
    if (!hp) {
        complete(iosb, XF_INVALID_HANDLE, 0);
        return XF_INVALID_HANDLE;
    }
    const int part = hp->mount ? hp->mount->partition : 0;
    uint8_t *p = (uint8_t *)out;
    switch (code) {
    case 0x70000: { // IOCTL_DISK_GET_DRIVE_GEOMETRY
        if (part < 0 || part > 5) return XF_INVALID_DEVICE_REQUEST;
        if (out_len < 24) {
            complete(iosb, XF_BUFFER_TOO_SMALL, 0);
            return XF_BUFFER_TOO_SMALL;
        }
        // DISK_GEOMETRY as NT lays it out, which is what the Xbox reports and what k_file.cpp fills:
        //   Cylinders (LARGE_INTEGER), MediaType, TracksPerCylinder, SectorsPerTrack, BytesPerSector.
        // The values come from the reference: removable media, one track of one sector, 512-byte sectors,
        // and a cylinder count that adds up to the partition's size.
        const PartitionGeom &g = kPartitions[part];
        const uint64_t cylinders = g.size / 512;
        memset(p, 0, 24);
        memcpy(p + 0, &cylinders, 8);
        *(uint32_t *)(p + 8) = 12;   // MediaType: RemovableMedia
        *(uint32_t *)(p + 12) = 1;   // TracksPerCylinder
        *(uint32_t *)(p + 16) = 1;   // SectorsPerTrack
        *(uint32_t *)(p + 20) = 512; // BytesPerSector
        complete(iosb, XF_SUCCESS, 24);
        return XF_SUCCESS;
    }
    case 0x74004: { // IOCTL_DISK_GET_PARTITION_INFO
        if (part < 0 || part > 5) return XF_INVALID_DEVICE_REQUEST;
        if (out_len < 16) {
            complete(iosb, XF_BUFFER_TOO_SMALL, 0);
            return XF_BUFFER_TOO_SMALL;
        }
        const PartitionGeom &g = kPartitions[part];
        memset(p, 0, 16);
        *(uint32_t *)(p + 4) = (uint32_t)(g.start / 512);  // StartingOffset, in sectors
        *(uint32_t *)(p + 8) = (uint32_t)(g.size / 512);   // PartitionLength
        complete(iosb, XF_SUCCESS, 16);
        return XF_SUCCESS;
    }
    default:
        complete(iosb, XF_INVALID_DEVICE_REQUEST, 0);
        return XF_INVALID_DEVICE_REQUEST;
    }
}