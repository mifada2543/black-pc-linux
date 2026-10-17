#include "kernel/k_file_posix.h"

#include "platform/posix/log.h"

#include <string.h>

namespace {

// Every Nt* call that takes an event handle and an APC routine completes synchronously here: the file
// layer is not asynchronous, so there is nothing to signal and no APC to queue. The game is written
// against a kernel that completes these inline too, so it passes null for both in the cases that matter.
// Saying it once per call, the first time, is enough.
bool g_apc_noted = false;
void note_apc(const char *what, void *apc_routine)
{
    if (!apc_routine || g_apc_noted) return;
    g_apc_noted = true;
    logf("file: %s with an APC routine: completed synchronously, no APC queued", what);
}

} // namespace

void kfile_init(const std::string &game_dir, const std::string &hdd_dir)
{
    xf_init(game_dir, hdd_dir);
    logc("file: \\Device\\CdRom0 -> %s (read-only), \\Device\\Harddisk0 -> %s", game_dir.c_str(), hdd_dir.c_str());
}

// ----------------------------------------------------------------------------------------------------------

extern "C" NTSTATUS __attribute__((stdcall)) NtCreateFile(uint32_t *FileHandle, uint32_t DesiredAccess,
                                                          void *ObjectAttributes, XIoStatusBlock *IoStatusBlock,
                                                          XLargeInteger *AllocationSize, uint32_t FileAttributes,
                                                          uint32_t ShareAccess, uint32_t CreateDisposition,
                                                          uint32_t CreateOptions)
{
    XfHandle h = XF_INVALID_HANDLE_VALUE;
    const NTSTATUS s = xf_create_file(&h, DesiredAccess, (XObjectAttributes *)ObjectAttributes, IoStatusBlock,
                                      AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions);
    if (FileHandle) *FileHandle = h;
    return s;
}

extern "C" NTSTATUS __attribute__((stdcall)) NtOpenFile(uint32_t *FileHandle, uint32_t DesiredAccess,
                                                        void *ObjectAttributes, XIoStatusBlock *IoStatusBlock,
                                                        uint32_t ShareAccess, uint32_t OpenOptions)
{
    XfHandle h = XF_INVALID_HANDLE_VALUE;
    const NTSTATUS s = xf_open_file(&h, DesiredAccess, (XObjectAttributes *)ObjectAttributes, ShareAccess,
                                    OpenOptions);
    if (FileHandle) *FileHandle = h;
    (void)IoStatusBlock; // xf_open_file completes nothing; it has no allocation or read to report
    return s;
}

extern "C" NTSTATUS __attribute__((stdcall)) NtReadFile(uint32_t FileHandle, uint32_t Event, void *ApcRoutine,
                                                        void *ApcContext, XIoStatusBlock *IoStatusBlock, void *Buffer,
                                                        uint32_t Length, XLargeInteger *ByteOffset)
{
    note_apc("NtReadFile", ApcRoutine);
    (void)Event;
    (void)ApcContext;
    return xf_read(FileHandle, Buffer, Length, ByteOffset, IoStatusBlock);
}

extern "C" NTSTATUS __attribute__((stdcall)) NtWriteFile(uint32_t FileHandle, uint32_t Event, void *ApcRoutine,
                                                         void *ApcContext, XIoStatusBlock *IoStatusBlock,
                                                         const void *Buffer, uint32_t Length,
                                                         XLargeInteger *ByteOffset)
{
    note_apc("NtWriteFile", ApcRoutine);
    (void)Event;
    (void)ApcContext;
    return xf_write(FileHandle, Buffer, Length, ByteOffset, IoStatusBlock);
}

extern "C" NTSTATUS __attribute__((stdcall)) NtFlushBuffersFile(uint32_t FileHandle, XIoStatusBlock *IoStatusBlock)
{
    const NTSTATUS s = xf_flush(FileHandle);
    if (IoStatusBlock) {
        IoStatusBlock->Status = s;
        IoStatusBlock->Information = 0;
    }
    return s;
}

extern "C" NTSTATUS __attribute__((stdcall)) NtQueryInformationFile(uint32_t FileHandle,
                                                                    XIoStatusBlock *IoStatusBlock, void *FileInformation,
                                                                    uint32_t Length, uint32_t FileInformationClass)
{
    return xf_query_information(FileHandle, FileInformationClass, FileInformation, Length, IoStatusBlock);
}

extern "C" NTSTATUS __attribute__((stdcall)) NtSetInformationFile(uint32_t FileHandle,
                                                                 XIoStatusBlock *IoStatusBlock, void *FileInformation,
                                                                 uint32_t Length, uint32_t FileInformationClass)
{
    return xf_set_information(FileHandle, FileInformationClass, FileInformation, Length, IoStatusBlock);
}

extern "C" NTSTATUS __attribute__((stdcall)) NtQueryVolumeInformationFile(uint32_t FileHandle,
                                                                          XIoStatusBlock *IoStatusBlock,
                                                                          void *FsInformation, uint32_t Length,
                                                                          uint32_t FsInformationClass)
{
    return xf_query_volume(FileHandle, FsInformation, Length, FsInformationClass, IoStatusBlock);
}

extern "C" NTSTATUS __attribute__((stdcall)) NtQueryDirectoryFile(uint32_t FileHandle, uint32_t Event,
                                                                  void *ApcRoutine, void *ApcContext,
                                                                  XIoStatusBlock *IoStatusBlock, void *FileInformation,
                                                                  uint32_t Length, uint32_t FileInformationClass,
                                                                  XAnsiString *FileName, uint8_t RestartScan)
{
    note_apc("NtQueryDirectoryFile", ApcRoutine);
    (void)Event;
    (void)ApcContext;
    // Class 1 is FileDirectoryInformation, the only shape the game asks for: a 64-byte header with an
    // 8-bit name at offset 64, which is what xfile.cpp fills in.
    if (FileInformationClass != 1) {
        logf("file: NtQueryDirectoryFile: class %u not implemented", FileInformationClass);
        if (IoStatusBlock) {
            IoStatusBlock->Status = XF_INVALID_PARAMETER;
            IoStatusBlock->Information = 0;
        }
        return XF_INVALID_PARAMETER;
    }
    const std::string mask = FileName ? xstr(FileName) : std::string();
    return xf_query_directory(FileHandle, FileInformation, Length, mask, RestartScan != 0, IoStatusBlock);
}

extern "C" NTSTATUS __attribute__((stdcall)) NtQueryFullAttributesFile(void *ObjectAttributes, void *FileInformation)
{
    return xf_query_full_attributes((XObjectAttributes *)ObjectAttributes, (XFileNetworkOpenInformation *)FileInformation);
}

extern "C" NTSTATUS __attribute__((stdcall)) NtDeviceIoControlFile(uint32_t FileHandle, uint32_t Event,
                                                                   void *ApcRoutine, void *ApcContext,
                                                                   XIoStatusBlock *IoStatusBlock, uint32_t IoControlCode,
                                                                   void *InputBuffer, uint32_t InputBufferLength,
                                                                   void *OutputBuffer, uint32_t OutputBufferLength)
{
    note_apc("NtDeviceIoControlFile", ApcRoutine);
    (void)Event;
    (void)ApcContext;
    (void)InputBuffer;
    (void)InputBufferLength;
    return xf_device_io_control(FileHandle, IoControlCode, OutputBuffer, OutputBufferLength, IoStatusBlock);
}

extern "C" NTSTATUS __attribute__((stdcall)) NtFsControlFile(uint32_t FileHandle, uint32_t Event, void *ApcRoutine,
                                                              void *ApcContext, XIoStatusBlock *IoStatusBlock,
                                                              uint32_t FsControlCode, void *InputBuffer,
                                                              uint32_t InputBufferLength, void *OutputBuffer,
                                                              uint32_t OutputBufferLength)
{
    note_apc("NtFsControlFile", ApcRoutine);
    (void)Event;
    (void)ApcContext;
    (void)InputBuffer;
    (void)InputBufferLength;
    (void)OutputBuffer;
    (void)OutputBufferLength;
    // Dismount is the only one the game issues, and on the console it flushes and marks the volume
    // read-only. Here the volume is already just a directory, so it succeeds and changes nothing.
    const NTSTATUS s = FsControlCode == KF_FSCTL_DISMOUNT_VOLUME ? XF_SUCCESS : XF_INVALID_DEVICE_REQUEST;
    logf("file: NtFsControlFile('%s', 0x%x) -> 0x%08x", xf_handle_desc(FileHandle).c_str(), FsControlCode, s);
    if (IoStatusBlock) {
        IoStatusBlock->Status = s;
        IoStatusBlock->Information = 0;
    }
    return s;
}

// ----------------------------------------------------------------------------------------------------------

extern "C" NTSTATUS __attribute__((stdcall)) IoCreateSymbolicLink(XAnsiString *SymbolicLinkName,
                                                                  XAnsiString *DeviceName)
{
    return xf_add_symlink(SymbolicLinkName ? xstr(SymbolicLinkName) : std::string(),
                          DeviceName ? xstr(DeviceName) : std::string());
}

extern "C" NTSTATUS __attribute__((stdcall)) IoDeleteSymbolicLink(XAnsiString *SymbolicLinkName)
{
    return xf_remove_symlink(SymbolicLinkName ? xstr(SymbolicLinkName) : std::string());
}

extern "C" NTSTATUS __attribute__((stdcall)) NtOpenSymbolicLinkObject(uint32_t *LinkHandle, void *ObjectAttributes)
{
    const XObjectAttributes *oa = (const XObjectAttributes *)ObjectAttributes;
    XfHandle h = XF_INVALID_HANDLE_VALUE;
    const NTSTATUS s = xf_open_symlink(&h, oa ? oa->ObjectName : nullptr);
    if (LinkHandle) *LinkHandle = h;
    return s;
}

extern "C" NTSTATUS __attribute__((stdcall)) NtQuerySymbolicLinkObject(uint32_t LinkHandle, XAnsiString *LinkTarget,
                                                                       uint32_t *ReturnedLength)
{
    return xf_query_symlink(LinkHandle, LinkTarget, ReturnedLength);
}