// Xbox file and object-path emulation on POSIX.
//
// This is the Linux half of loader/src/kernel/k_file.cpp. The Xbox-visible model is unchanged --
// 8-bit object paths, the symbolic link table, the mount table, FATX-shaped volume numbers, raw
// volumes, one directory entry per query -- because the game reads all of that and it must not move.
// Only the leaf operations are different: 21 ntdll entry points become POSIX calls.
//
// Two things genuinely improve rather than merely port:
//
//   * host paths are std::string UTF-8, not std::wstring. The Windows build widens Xbox bytes to
//     wchar_t one byte at a time (k_file.cpp ansi_to_wide), which mangles any non-ASCII file name.
//     Here the bytes pass through unchanged, and only the host's own UTF-8 names are decoded.
//   * handles are a small index table rather than real OS handles. There is no Windows handle to
//     borrow, and a plain index is cheaper and easier to reason about than an opaque pointer.
//
// Object paths resolve in two steps, exactly as on Windows: the symbolic link table turns
// "\??\D:\media\x.bin" into "\Device\CdRom0\media\x.bin", then the longest matching device prefix
// picks a mount and the remainder is appended to that mount's host folder.
#pragma once

#include <stdint.h>

#include <string>

// ---- Xbox kernel ABI pieces this file needs, defined locally ----
// common.h declares these too, but it includes <windows.h> for the whole loader. Keeping them here
// means the file layer can be built and tested on its own.

using NTSTATUS = int32_t;

#define XF_SUCCESS ((NTSTATUS)0x00000000)
#define XF_PENDING ((NTSTATUS)0x00000103)
#define XF_NO_MORE_FILES ((NTSTATUS)0x80000006)
#define XF_UNSUCCESSFUL ((NTSTATUS)0xC0000001)
#define XF_NOT_IMPLEMENTED ((NTSTATUS)0xC0000002)
#define XF_INVALID_HANDLE ((NTSTATUS)0xC0000008)
#define XF_INVALID_PARAMETER ((NTSTATUS)0xC000000D)
#define XF_INVALID_DEVICE_REQUEST ((NTSTATUS)0xC0000010)
#define XF_NO_MEMORY ((NTSTATUS)0xC0000017)
#define XF_ACCESS_DENIED ((NTSTATUS)0xC0000022)
#define XF_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023)
#define XF_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034)
#define XF_OBJECT_NAME_COLLISION ((NTSTATUS)0xC0000035)
#define XF_OBJECT_PATH_NOT_FOUND ((NTSTATUS)0xC000003A)
#define XF_NOT_SUPPORTED ((NTSTATUS)0xC00000BB)
#define XF_NOT_A_DIRECTORY ((NTSTATUS)0xC0000103)
#define XF_BUFFER_OVERFLOW ((NTSTATUS)0x80000005)
#define XF_END_OF_FILE ((NTSTATUS)0xC0000011)

// FILE_INFORMATION_CLASS / FS_INFORMATION_CLASS values used below.
enum {
    XF_FileDirectoryInformation = 1,
    XF_FileFullDirectoryInformation = 2,
    XF_FileBothDirectoryInformation = 3,
    XF_FileRenameInformation = 10,
    XF_FileDispositionInformation = 13,
    XF_FileEndOfFileInformation = 20,
    XF_FileNetworkOpenInformation = 34,
};
enum {
    XF_FileFsVolumeInformation = 1,
    XF_FileFsSizeInformation = 3,
    XF_FileFsAttributeInformation = 5,
    XF_FileFsFullSizeInformation = 7,
};

#define XF_NT_OK(s) ((s) >= 0)

struct XAnsiString {
    uint16_t Length;
    uint16_t MaximumLength;
    char *Buffer;
};

// NT's IO_STATUS_BLOCK is two words: the status (the kernel also stores a pointer there when an APC
// will complete the request) and the number of bytes transferred. They are separate fields, not a
// union -- the game reads Information after every read, and a union of the two silently replaced the
// status with the byte count and left a stale value where the count should be.
struct XIoStatusBlock {
    union {
        NTSTATUS Status;
        void *Pointer;
    };
    uint32_t Information;
};

struct XObjectAttributes {
    uint32_t RootDirectory;
    XAnsiString *ObjectName;
    uint32_t Attributes;
};

struct XLargeInteger {
    union {
        struct {
            uint32_t LowPart;
            int32_t HighPart;
        };
        int64_t QuadPart;
    };
};

// FILE_DIRECTORY_INFORMATION with an 8-bit name, which is what the Xbox game code reads.
struct XFileDirectoryInformation {
    uint32_t NextEntryOffset;
    uint32_t FileIndex;
    XLargeInteger CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
    XLargeInteger EndOfFile, AllocationSize;
    uint32_t FileAttributes;
    uint32_t FileNameLength;
    char FileName[1];
};

struct XFileNetworkOpenInformation {
    XLargeInteger CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
    XLargeInteger AllocationSize, EndOfFile;
    uint32_t FileAttributes;
    uint32_t Reserved;
};

struct XFileFullDirectoryInformation {
    XFileDirectoryInformation basic;
    XLargeInteger EaSize;
};

// ---- the layer ----

// Handles. Small indices, so a bad handle is caught by range instead of by dereferencing garbage.
using XfHandle = uint32_t;
constexpr XfHandle XF_INVALID_HANDLE_VALUE = 0xFFFFFFFFu;

// CreateDisposition / CreateOptions / CreateOptions values, matching the Xbox kernel's.
enum {
    XF_FILE_SUPERSEDE = 0,
    XF_FILE_OPEN = 1,
    XF_FILE_CREATE = 2,
    XF_FILE_OPEN_IF = 3,
    XF_FILE_OVERWRITE = 4,
    XF_FILE_OVERWRITE_IF = 5,
};
enum {
    XF_DIRECTORY_FILE = 0x00000001,
    XF_WRITE_THROUGH = 0x00000002,
    XF_SEQUENTIAL_ONLY = 0x00000004,
    XF_NO_INTERMEDIATE_BUFFERING = 0x00000008,
    XF_DELETE_ON_CLOSE = 0x00001000,
    XF_NON_DIRECTORY_FILE = 0x00000040,
};
enum {
    XF_FILE_READ_DATA = 0x00000001,
    XF_FILE_WRITE_DATA = 0x00000002,
    XF_FILE_APPEND_DATA = 0x00000004,
    XF_FILE_READ_EA = 0x00000008,
    XF_FILE_WRITE_EA = 0x00000010,
    XF_FILE_READ_ATTRIBUTES = 0x00000080,
    XF_FILE_WRITE_ATTRIBUTES = 0x00000100,
    XF_FILE_LIST_DIRECTORY = 0x00000001,
    XF_GENERIC_READ = 0x80000000,
    XF_GENERIC_WRITE = 0x40000000,
    XF_GENERIC_ALL = 0x10000000,
    XF_DELETE = 0x00010000,
    XF_SYNCHRONIZE = 0x00100000,
};

// Build the mount and link tables. game_dir backs \Device\CdRom0 (read-only), hdd_dir backs
// \Device\Harddisk0 and Partition1-5. Creates the partition folders and Partition1's TDATA/UDATA.
void xf_init(const std::string &game_dir, const std::string &hdd_dir);

// Symbolic links, as ObReferenceObjectByHandle on \??\D: does on Windows. Names are case-insensitive.
// Adding a name that already exists reports a name collision rather than replacing it.
NTSTATUS xf_add_symlink(const std::string &name, const std::string &target);
NTSTATUS xf_remove_symlink(const std::string &name);
// Opening a link yields a handle of its own (kind XHANDLE_LINK) that reports the target, because the
// Xbox's link object does too and the game asks for it with NtQuerySymbolicLinkObject.
NTSTATUS xf_open_symlink(XfHandle *out, const XAnsiString *name);
NTSTATUS xf_query_symlink(XfHandle h, XAnsiString *target, uint32_t *returned_length);

// The host folder behind an \Device\* mount point, empty when the path is not one. Matching is exact,
// because \Device\Harddisk0 on its own is not a mount point (Partition0..5 are) and answering for it
// anyway would hide which partition a path really lands in.
std::string xf_device_host_path(const char *xbox_device_path);

// Canonical Xbox path a handle refers to, for logging. "?" when the handle is not known.
std::string xf_handle_desc(XfHandle h);

// Number of live file handles, for leak checks in the tests.
uint32_t xf_open_handle_count();

NTSTATUS xf_create_file(XfHandle *out, uint32_t access, XObjectAttributes *oa, XIoStatusBlock *iosb, XLargeInteger *alloc,
                        uint32_t attrs, uint32_t share, uint32_t disp, uint32_t opts);
NTSTATUS xf_open_file(XfHandle *out, uint32_t access, XObjectAttributes *oa, uint32_t share, uint32_t opts);
NTSTATUS xf_close(XfHandle h);
NTSTATUS xf_duplicate(XfHandle src, XfHandle *out, uint32_t options);

// ByteOffset == nullptr means "use the handle's own position", which is what a sequential Xbox file
// does. Raw volumes read as zeroes past their end rather than failing.
NTSTATUS xf_read(XfHandle h, void *buf, uint32_t len, XLargeInteger *offset, XIoStatusBlock *iosb);
NTSTATUS xf_write(XfHandle h, const void *buf, uint32_t len, XLargeInteger *offset, XIoStatusBlock *iosb);

NTSTATUS xf_flush(XfHandle h);
NTSTATUS xf_set_information_file(XfHandle h, uint32_t class_, const std::string &new_name);

// The information classes the Xbox kernel answers for a file. Only the ones the game can need are
// implemented -- see xfile.cpp for which numbers those are and why two are accepted for a rename -- and
// anything else reports XF_INVALID_PARAMETER rather than filling in a guess.
NTSTATUS xf_query_information(XfHandle h, uint32_t class_, void *info, uint32_t len, XIoStatusBlock *iosb);
NTSTATUS xf_set_information(XfHandle h, uint32_t class_, const void *info, uint32_t len, XIoStatusBlock *iosb);
NTSTATUS xf_delete(XfHandle h); // FileDispositionInformation: mark delete-on-close, do it at once

NTSTATUS xf_query_volume(XfHandle h, void *info, uint32_t len, uint32_t class_, XIoStatusBlock *iosb);

// One entry per call, 8-bit name, "." and ".." skipped, NextEntryOffset zeroed. Returns
// XF_NO_MORE_FILES at the end of the directory.
NTSTATUS xf_query_directory(XfHandle h, void *info, uint32_t len, const std::string &mask, bool restart, XIoStatusBlock *iosb);

NTSTATUS xf_query_full_attributes(XObjectAttributes *oa, XFileNetworkOpenInformation *out);
NTSTATUS xf_query_directory_attributes(XObjectAttributes *oa, XFileFullDirectoryInformation *out);

// IOCTL_DISK_GET_DRIVE_GEOMETRY (0x70000) and IOCTL_DISK_GET_PARTITION_INFO (0x74004), which the game
// issues before it reads the save partition.
NTSTATUS xf_device_io_control(XfHandle h, uint32_t code, void *out, uint32_t out_len, XIoStatusBlock *iosb);

std::string xstr(const XAnsiString *s);