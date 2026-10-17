// One handle space for everything the game can hold a handle to.
//
// The Xbox hands out handles from a single process-wide table, so a game can pass any handle to NtClose
// or NtWaitForSingleObject without saying what it is. Our layers keep their own arenas -- the file
// layer's entries carry host fds and mounts, the sync layer's carry condition variables -- so a handle
// has to say which arena it belongs to, or NtClose would close the wrong object when a file handle and an
// event handle happened to be the same small integer.
//
// The kind lives in the top four bits and the arena index in the low 28. That leaves room for 268 million
// entries per kind, which is far more than any arena needs, and it makes the invalid value work for free:
// 0xFFFFFFFF has kind 15, and no kind is 15.
//
// This header is deliberately free of state. The table that decides what a handle *means* is the object
// manager's, and until the thread layer lands it is NtClose's dispatch in k_sync_posix.cpp.
#pragma once

#include <stdint.h>

// The kinds, numbered so that 0xFFFFFFFF (all bits set) is not one of them.
enum : uint32_t {
    XHANDLE_NONE = 0,
    XHANDLE_SYNC = 1,  // events and semaphores: xsync.cpp
    XHANDLE_FILE = 2,  // files and directories: xfile.cpp
    XHANDLE_LINK = 3,  // symbolic-link objects: xfile.cpp
    XHANDLE_THREAD = 4 // threads: k_thread, when it lands
};

constexpr uint32_t XHANDLE_KIND_SHIFT = 28;
constexpr uint32_t XHANDLE_KIND_MASK = 0xF0000000u;
constexpr uint32_t XHANDLE_INDEX_MASK = 0x0FFFFFFFu;

// A handle whose index is too large to exist, and the value both layers already use for "no handle".
constexpr uint32_t XHANDLE_NONE_VALUE = 0xFFFFFFFFu;

constexpr uint32_t xhandle_make(uint32_t kind, uint32_t index)
{
    return ((kind << XHANDLE_KIND_SHIFT) & XHANDLE_KIND_MASK) | (index & XHANDLE_INDEX_MASK);
}
constexpr uint32_t xhandle_kind(uint32_t h) { return (h & XHANDLE_KIND_MASK) >> XHANDLE_KIND_SHIFT; }
constexpr uint32_t xhandle_index(uint32_t h) { return h & XHANDLE_INDEX_MASK; }
constexpr bool xhandle_is(uint32_t h, uint32_t kind) { return xhandle_kind(h) == kind; }

// True when the value is a handle of some known kind, i.e. not zero and not 0xFFFFFFFF.
constexpr bool xhandle_valid(uint32_t h)
{
    const uint32_t k = xhandle_kind(h);
    return k >= XHANDLE_SYNC && k <= XHANDLE_THREAD && h != XHANDLE_NONE_VALUE;
}

// For a log line: what a handle is, and what arena index it has.
const char *xhandle_kind_name(uint32_t h);