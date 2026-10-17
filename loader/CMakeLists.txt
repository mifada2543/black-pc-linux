cmake_minimum_required(VERSION 3.24)
if(POLICY CMP0184)
  cmake_policy(SET CMP0184 NEW) # MSVC_RUNTIME_CHECKS property
endif()
project(black_loader VERSION 0.2.0 LANGUAGES C CXX) # the release number (About box, first log line)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Linux (i386) -- the port under development.
#
# The loader must be a 32-bit process: long mode cannot execute the XBE's x86-32 code, and the game
# bakes absolute addresses into it. There is no stub and no PE header splice here (see .agents/plan.md
# section 3.1) -- the image range is a plain mmap and the entry point is main().
# ---------------------------------------------------------------------------
if(UNIX)
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "The loader must be 32-bit: configure with -DCMAKE_CXX_FLAGS=-m32 (the 'linux' preset does this).")
  endif()

  add_library(blackcore STATIC
    src/platform/posix/teb.cpp
    src/platform/posix/seh.cpp
    src/platform/posix/fs_patch.cpp
    src/platform/posix/xfile.cpp
    src/platform/posix/xbe.cpp
    src/platform/posix/image.cpp
    src/platform/posix/xsync.cpp
    src/platform/posix/log.cpp
    src/platform/posix/boot.cpp
    src/kernel/ktable_posix.cpp
    src/kernel/k_memory_posix.cpp
    src/kernel/k_misc_posix.cpp
    src/kernel/k_sync_posix.cpp
    src/kernel/k_rtl_posix.cpp
    src/kernel/k_file_posix.cpp
    src/sha1.cpp)
  target_include_directories(blackcore PUBLIC src)
  # Large-file support: the emulated Xbox disk is up to 8 GB and a 32-bit off_t wraps past 2 GB.
  target_compile_definitions(blackcore PUBLIC _FILE_OFFSET_BITS=64)
  target_compile_options(blackcore PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(blackcore PUBLIC pthread)

  # The loader itself. There is no stub and no DLL on Linux: the program maps the image over its own
  # address space, so main() is the whole entry point (src/platform/posix/boot.h has the sequence).
  add_executable(blacklinux src/platform/posix/main.cpp)
  target_link_libraries(blacklinux PRIVATE blackcore)

  # Platform layer tests. These need no XBE: they exercise the TEB anchor, the fs->gs patcher and the
  # SEH chain directly.
  enable_testing()
  add_executable(test_platform tests/test_platform.cpp)
  target_link_libraries(test_platform PRIVATE blackcore)
  add_test(NAME platform COMMAND test_platform)

  # The Xbox file layer, driven against a real temporary filesystem tree standing in for the disc and
  # the hard disk.
  add_executable(test_xfile tests/test_xfile.cpp)
  target_link_libraries(test_xfile PRIVATE blackcore)
  add_test(NAME xfile COMMAND test_xfile)

  # XBE parsing and image mapping, driven by a synthetic image whose code section is really executed
  # after the fs->gs patch.
  add_executable(test_xbe tests/test_xbe.cpp)
  target_link_libraries(test_xbe PRIVATE blackcore)
  add_test(NAME xbe COMMAND test_xbe)

  # Dispatcher objects, critical sections and IRQL. Real threads and real timeouts.
  add_executable(test_xsync tests/test_xsync.cpp)
  target_link_libraries(test_xsync PRIVATE blackcore)
  add_test(NAME xsync COMMAND test_xsync)

  # The Phase 1 boot path against a synthetic image: map, copy, patch, resolve 115 imports, then run
  # the image's entry point on a real thread. This is the gate the real XBE will have to pass too.
  add_executable(test_boot tests/test_boot.cpp)
  target_link_libraries(test_boot PRIVATE blackcore)
  add_test(NAME boot COMMAND test_boot)

  # The Xbox memory layer: pool, contiguous memory, virtual memory. Includes a deliberate fault on a
  # protected page, so it exercises the fault bridge as well.
  add_executable(test_memory tests/test_memory.cpp)
  target_link_libraries(test_memory PRIVATE blackcore)
  add_test(NAME memory COMMAND test_memory)

  # The Nt* file surface: creation, reading, writing, the information classes, rename, directory
  # enumeration, ioctls and symbolic links, all through the Xbox's own calls.
  add_executable(test_kfile tests/test_kfile.cpp)
  target_link_libraries(test_kfile PRIVATE blackcore)
  add_test(NAME kfile COMMAND test_kfile)

  # The Rtl* exports: strings, time (checked against gmtime_r), status mapping, critical sections.
  add_executable(test_krtl tests/test_krtl.cpp)
  target_link_libraries(test_krtl PRIVATE blackcore)
  add_test(NAME krtl COMMAND test_krtl)

  # The Xbox synchronisation surface: events, semaphores, waits across threads, IRQL, dispatcher objects.
  add_executable(test_ksync tests/test_ksync.cpp)
  target_link_libraries(test_ksync PRIVATE blackcore)
  add_test(NAME ksync COMMAND test_ksync)

  # The handle space both file and sync objects live in: two arenas, one value space.
  add_executable(test_handle tests/test_handle.cpp)
  target_link_libraries(test_handle PRIVATE blackcore)
  add_test(NAME handle COMMAND test_handle)

  # The AV/HAL/interrupt/settings/section/crypto exports, the data exports, DbgPrint's format
  # translation, and the three calls that have to end the run (checked in a forked child, so a bugcheck
  # cannot take the suite with it).
  add_executable(test_misc tests/test_misc.cpp)
  target_link_libraries(test_misc PRIVATE blackcore)
  add_test(NAME misc COMMAND test_misc)

  # Proves Vulkan works from a 32-bit process and records which features the backend may rely on.
  find_package(Vulkan QUIET)
  if(Vulkan_FOUND)
    message(STATUS "Vulkan ${Vulkan_VERSION} found -- building the 32-bit probe")
    add_executable(test_vulkan tests/test_vulkan.cpp)
    target_link_libraries(test_vulkan PRIVATE Vulkan::Vulkan)
    add_test(NAME vulkan COMMAND test_vulkan)
  else()
    message(WARNING "Vulkan not found -- skipping the 32-bit probe")
  endif()
  return()
endif()

# ---------------------------------------------------------------------------
# Windows / MSVC. Kept as the reference implementation of the HLE layers (see .agents/plan.md
# section 1: the Windows port is archived, not maintained, but not yet replaced).
# ---------------------------------------------------------------------------
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "The loader must be 32-bit: configure with -A Win32 (or use the 'win32' preset).")
endif()
if(NOT MSVC)
  message(FATAL_ERROR "MSVC (cl or clang-cl) is required: the loader uses MSVC x86 inline asm and linker options.")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# blackrt.dll: XBE loader, HLE kernel, hooks. Everything that is real code lives here.
add_library(blackrt SHARED
  src/main.cpp
  src/log.cpp
  src/xbe.cpp
  src/image.cpp
  src/xthread.cpp
  src/hooks.cpp
  src/timing.cpp
  src/fov.cpp
  src/fe/calls.cpp
  src/fe/vars.cpp
  src/fe/pc_settings.cpp
  src/fe/apt.cpp
  src/fe/pages.cpp
  src/fe/bind_list.cpp
  src/fe/pause_ui.cpp
  src/input/mouselook.cpp
  src/input/autoaim.cpp
  src/input/bindings.cpp
  src/input/movies.cpp
  src/audio/reverb.cpp
  src/settings.cpp
  src/perf.cpp
  src/sha1.cpp
  src/kernel/ktable.cpp
  src/kernel/k_memory.cpp
  src/kernel/k_thread.cpp
  src/kernel/k_sync.cpp
  src/kernel/k_dpc.cpp
  src/kernel/k_file.cpp
  src/kernel/k_rtl.cpp
  src/kernel/k_misc.cpp
  src/d3d/dispatch.cpp
  src/d3d/device.cpp
  src/d3d/host.cpp
  src/d3d/pushbuf.cpp
  src/d3d/draw.cpp
  src/d3d/formats.cpp
  src/d3d/resources.cpp
  src/d3d/texture.cpp
  src/d3d/vshader.cpp
  src/d3d/nv2a_vsh.cpp
  src/d3d/nv2a_psh.cpp
  src/d3d/pshader.cpp
  src/audio/dsound.cpp
  src/audio/output.cpp
  src/input/xinput.cpp
)
target_include_directories(blackrt PRIVATE src)
target_compile_definitions(blackrt PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE
  BLACK_VERSION="${PROJECT_VERSION}")
target_compile_options(blackrt PRIVATE /W3 /utf-8 /EHsc)
target_link_libraries(blackrt PRIVATE ntdll bcrypt d3d9 d3dcompiler windowscodecs ole32 user32 gdi32 winmm dbghelp advapi32 shell32)
# Xbox SEH handlers run through the same fs:[0] chain; no SafeSEH tables anywhere in the process.
target_link_options(blackrt PRIVATE /SAFESEH:NO /MAP) # blackrt.map: port addresses in crash logs -> functions

# black.exe: tiny stub with no CRT. Linked at 0x10000 with a large BSS so its own image owns the
# XBE's address range (0x10000-0x587a40) before Windows can place heaps or stacks there.
# blackrt.dll then maps the XBE over this image (headers included) and never returns.
# A Windows-subsystem program: no console window; started from a command prompt it attaches to that one (main.cpp).
add_executable(black stub/stub.c)
target_link_libraries(black PRIVATE blackrt kernel32)
set_property(TARGET black PROPERTY MSVC_RUNTIME_CHECKS "")
target_compile_options(black PRIVATE /GS- /Gs999999)
target_link_options(black PRIVATE
  /BASE:0x10000 /FIXED /DYNAMICBASE:NO /NXCOMPAT:NO /SAFESEH:NO /LARGEADDRESSAWARE
  /NODEFAULTLIB /ENTRY:StubEntry /SUBSYSTEM:WINDOWS /MANIFEST:NO /INCREMENTAL:NO)
