// The Nt* file exports, wired to the POSIX file layer in xfile.{h,cpp}.
//
// These are almost entirely shape: the layer underneath already does the work, and what is added here is
// the argument order the Xbox kernel uses (which is not Win32's), the IO_STATUS_BLOCK each call fills,
// and the two arguments every asynchronous-looking Nt* call carries -- the event handle and the APC
// routine -- which are accepted and ignored. On the Xbox those calls complete synchronously against a
// cached filesystem, so the game never has an APC to queue; saying so once is better than pretending.
//
// Two classes are worth naming because they are guesses that could not be checked without the real image:
//
//   * FILE_INFORMATION_CLASS numbering. The reference passes most classes straight through to the Windows
//     host and synthesises only FileNameInformation, which it numbers 9. xfile.cpp answers 4, 5, 9 and 14,
//     and accepts a rename as either 7 (NT's number) or 10 (the reference's). A rename request that does
//     not resolve changes nothing, so an ambiguous number costs a status rather than a file.
//   * Every unknown class logs the number it was asked for and returns STATUS_INVALID_PARAMETER. When
//     default.xbe arrives, the first run's log says exactly which classes the game wants.
#pragma once

#include <stdint.h>

#include "platform/posix/xfile.h" // XfHandle, XObjectAttributes, XIoStatusBlock, XLargeInteger, XAnsiString

// FSCTL_DISMOUNT_VOLUME, the one filesystem control the Xbox game code issues.
#define KF_FSCTL_DISMOUNT_VOLUME 0x00090020u

extern "C" NTSTATUS __attribute__((stdcall)) NtCreateFile(uint32_t *FileHandle, uint32_t DesiredAccess,
                                                          void *ObjectAttributes, XIoStatusBlock *IoStatusBlock,
                                                          XLargeInteger *AllocationSize, uint32_t FileAttributes,
                                                          uint32_t ShareAccess, uint32_t CreateDisposition,
                                                          uint32_t CreateOptions);
extern "C" NTSTATUS __attribute__((stdcall)) NtOpenFile(uint32_t *FileHandle, uint32_t DesiredAccess,
                                                        void *ObjectAttributes, XIoStatusBlock *IoStatusBlock,
                                                        uint32_t ShareAccess, uint32_t OpenOptions);
extern "C" NTSTATUS __attribute__((stdcall)) NtReadFile(uint32_t FileHandle, uint32_t Event, void *ApcRoutine,
                                                        void *ApcContext, XIoStatusBlock *IoStatusBlock, void *Buffer,
                                                        uint32_t Length, XLargeInteger *ByteOffset);
extern "C" NTSTATUS __attribute__((stdcall)) NtWriteFile(uint32_t FileHandle, uint32_t Event, void *ApcRoutine,
                                                         void *ApcContext, XIoStatusBlock *IoStatusBlock,
                                                         const void *Buffer, uint32_t Length,
                                                         XLargeInteger *ByteOffset);
extern "C" NTSTATUS __attribute__((stdcall)) NtFlushBuffersFile(uint32_t FileHandle, XIoStatusBlock *IoStatusBlock);
extern "C" NTSTATUS __attribute__((stdcall)) NtQueryInformationFile(uint32_t FileHandle,
                                                                    XIoStatusBlock *IoStatusBlock, void *FileInformation,
                                                                    uint32_t Length, uint32_t FileInformationClass);
extern "C" NTSTATUS __attribute__((stdcall)) NtSetInformationFile(uint32_t FileHandle,
                                                                 XIoStatusBlock *IoStatusBlock, void *FileInformation,
                                                                 uint32_t Length, uint32_t FileInformationClass);
extern "C" NTSTATUS __attribute__((stdcall)) NtQueryVolumeInformationFile(uint32_t FileHandle,
                                                                          XIoStatusBlock *IoStatusBlock,
                                                                          void *FsInformation, uint32_t Length,
                                                                          uint32_t FsInformationClass);
extern "C" NTSTATUS __attribute__((stdcall)) NtQueryDirectoryFile(uint32_t FileHandle, uint32_t Event,
                                                                  void *ApcRoutine, void *ApcContext,
                                                                  XIoStatusBlock *IoStatusBlock, void *FileInformation,
                                                                  uint32_t Length, uint32_t FileInformationClass,
                                                                  XAnsiString *FileName, uint8_t RestartScan);
extern "C" NTSTATUS __attribute__((stdcall)) NtQueryFullAttributesFile(void *ObjectAttributes, void *FileInformation);
extern "C" NTSTATUS __attribute__((stdcall)) NtDeviceIoControlFile(uint32_t FileHandle, uint32_t Event,
                                                                   void *ApcRoutine, void *ApcContext,
                                                                   XIoStatusBlock *IoStatusBlock, uint32_t IoControlCode,
                                                                   void *InputBuffer, uint32_t InputBufferLength,
                                                                   void *OutputBuffer, uint32_t OutputBufferLength);
extern "C" NTSTATUS __attribute__((stdcall)) NtFsControlFile(uint32_t FileHandle, uint32_t Event, void *ApcRoutine,
                                                              void *ApcContext, XIoStatusBlock *IoStatusBlock,
                                                              uint32_t FsControlCode, void *InputBuffer,
                                                              uint32_t InputBufferLength, void *OutputBuffer,
                                                              uint32_t OutputBufferLength);

extern "C" NTSTATUS __attribute__((stdcall)) IoCreateSymbolicLink(XAnsiString *SymbolicLinkName,
                                                                  XAnsiString *DeviceName);
extern "C" NTSTATUS __attribute__((stdcall)) IoDeleteSymbolicLink(XAnsiString *SymbolicLinkName);
extern "C" NTSTATUS __attribute__((stdcall)) NtOpenSymbolicLinkObject(uint32_t *LinkHandle, void *ObjectAttributes);
extern "C" NTSTATUS __attribute__((stdcall)) NtQuerySymbolicLinkObject(uint32_t LinkHandle, XAnsiString *LinkTarget,
                                                                       uint32_t *ReturnedLength);

// The game directory is the folder the XBE came from, and the hard disk is a writable folder beside it.
// boot_prepare sets both up; this is here so the tests can set up their own.
void kfile_init(const std::string &game_dir, const std::string &hdd_dir);