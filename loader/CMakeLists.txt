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
# This file builds the Linux port and nothing else. The archived Windows port keeps its own build file next
# door (loader/windows/CMakeLists.txt), and it is deliberately not in git.
if(UNIX)
  if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "The loader must be 32-bit: configure with -DCMAKE_CXX_FLAGS=-m32 (the 'linux' preset does this).")
  endif()
  # ---- the window: SDL2, i386 ----
  #
  # Found by path rather than by pkg-config, because pkg-config on this machine answers with the amd64
  # package: a 64-bit libSDL2 is invisible to a 32-bit process, and the failure that comes from that is a
  # link error about SDL_Init in a build that "found" the library.
  #
  # Not required. The kernel layer, the D3D8 ABI and the two dozen tests that have nothing to do with
  # windows all build without it; only --window and the window test need it, and window_stub.cpp refuses
  # them by name when it is absent. That is the same trade the glslang block below makes, for the same
  # reason: a machine missing one package should still be able to run the suites that do not need it.
  find_path(SDL2_INCLUDE_DIR SDL.h
            PATHS /usr/include/SDL2 /usr/include/i386-linux-gnu/SDL2
            NO_CMAKE_FIND_ROOT_PATH)
  find_library(SDL2_LIBRARY NAMES SDL2 SDL2-2.0
               PATHS /usr/lib/i386-linux-gnu /usr/lib32 /usr/lib
               NO_CMAKE_FIND_ROOT_PATH)
  # find_library caches, so a package that is removed since the last configure leaves the path behind and
  # the build goes on believing in a file that is no longer there -- and then fails to link with a message
  # about SDL_Init. Re-checking the file is one line.
  if(SDL2_LIBRARY AND NOT EXISTS "${SDL2_LIBRARY}")
    message(STATUS "the SDL2 library was removed since the last configure -- not using the cached path")
    unset(SDL2_LIBRARY CACHE)
  endif()
  if(SDL2_INCLUDE_DIR AND SDL2_LIBRARY)
    message(STATUS "SDL2 ${SDL2_LIBRARY} -- the window path will be built")
    set(BLACKLINUX_WINDOW_SRC src/platform/posix/window.cpp src/platform/posix/timing.cpp)
    add_library(blacklinux_sdl INTERFACE)
    target_include_directories(blacklinux_sdl INTERFACE ${SDL2_INCLUDE_DIR})
    target_link_libraries(blacklinux_sdl INTERFACE ${SDL2_LIBRARY} ${CMAKE_DL_LIBS})
    # The whole block below is appended in one piece so that the source list and the link libraries cannot
    # disagree about whether the window exists.
    set(BLACKLINUX_SDL_TARGET blacklinux_sdl)
  else()
    message(WARNING
      "libsdl2-dev:i386 was not found, so the window path is disabled and --window refuses by name.\n"
      "  Install it with:  sudo apt install libsdl2-dev:i386\n"
      "  The x86-64 package does not help: this loader is a 32-bit process.\n"
      "  Everything else here builds and tests without it.")
    set(BLACKLINUX_WINDOW_SRC src/platform/posix/window_stub.cpp)
    set(BLACKLINUX_SDL_TARGET "")
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
    src/platform/posix/options.cpp
    src/platform/posix/boot.cpp
    ${BLACKLINUX_WINDOW_SRC}
    src/kernel/ktable_posix.cpp
    src/kernel/k_memory_posix.cpp
    src/kernel/k_misc_posix.cpp
    src/kernel/k_sync_posix.cpp
    src/kernel/k_rtl_posix.cpp
    src/kernel/k_file_posix.cpp
    src/kernel/k_thread_posix.cpp
    src/kernel/k_dpc_posix.cpp
    src/d3d/d3d_hooks_posix.cpp
    src/d3d/xd3d8_posix.cpp
    src/d3d/thunk_posix.cpp
    src/d3d/device_posix.cpp
    src/d3d/hle_table_posix.cpp
    src/d3d/hle_device.cpp
    src/d3d/resource_posix.cpp
    src/d3d/vshader_store_posix.cpp
    src/d3d/pipeline_cache_posix.cpp
    src/d3d/draw_posix.cpp
    src/d3d/vertex_posix.cpp
    src/d3d/texture_posix.cpp
    src/d3d/draw_material_posix.cpp
    src/input/xinput_posix.cpp
    src/input/bindings_posix.cpp
    src/input/mouselook_posix.cpp
    src/input/input_detail_posix.cpp
    src/audio/output_posix.cpp
    src/audio/reverb_posix.cpp
    src/audio/dsound_posix.cpp
    src/audio/decoder_posix.cpp
    # src/audio/stream_decode_posix.cpp is deliberately absent: it wraps the decoders in
    # decoder_posix.cpp, and without FFmpeg those are null-returning stubs, so this file would wrap
    # nothing. It is added below, under the same HAVE_FFMPEG test that decides whether A9 is built.
    src/shader/nv2a_vsh_posix.cpp
    src/shader/nv2a_psh_posix.cpp
    src/shader/material_posix.cpp
    src/shader/ffp_posix.cpp
    src/sha1.cpp)
  target_include_directories(blackcore PUBLIC src ${CMAKE_CURRENT_BINARY_DIR}/generated)
  # Large-file support: the emulated Xbox disk is up to 8 GB and a 32-bit off_t wraps past 2 GB.
  target_compile_definitions(blackcore PUBLIC _FILE_OFFSET_BITS=64)
  target_compile_options(blackcore PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(blackcore PUBLIC pthread)
  # Only when SDL2 was found; empty otherwise, which is what the stub was written for.
  if(BLACKLINUX_SDL_TARGET)
    target_link_libraries(blackcore PUBLIC ${BLACKLINUX_SDL_TARGET})
  endif()
  # ---- graphics: the Vulkan device the D3D8 layer will draw with, and the PNG writer ----
  #
  # Vulkan is found from the i386 development package, so the loader links the same 32-bit loader the game
  # runs against. There is no software fallback: a machine with no Vulkan driver cannot draw, and saying so
  # once at start-up is better than a black window.
  find_package(Vulkan REQUIRED)
  message(STATUS "Vulkan ${Vulkan_VERSION} -- the graphics layer will link ${Vulkan_LIBRARIES}")
  add_library(gfx STATIC
    src/gfx/gfx_vulkan.cpp
    src/gfx/png.cpp)
  target_include_directories(gfx PUBLIC src)
  target_link_libraries(gfx PUBLIC Vulkan::Vulkan)
  # gfx writes to the loader log, and blackcore links gfx, so this is a cycle between two static libraries.
  # Declared rather than left to the linker's order: without it a new target that links blackcore fails with
  # an undefined logf, and the failure says nothing about what went wrong.
  target_link_libraries(gfx PRIVATE blackcore)
  target_compile_options(gfx PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_libraries(blackcore PUBLIC gfx)
  # ---- the shader runtime: libglslang, i386, linked (plan section 7, decision 10) ----
  #
  # This is a runtime dependency and not the build-time one below: the game's NV2A microcode arrives while
  # it is running, so something has to compile it then. The x86-64 library would be useless here -- this
  # loader is a 32-bit process -- so the 32-bit one is looked for explicitly, and its absence is reported as
  # a sentence rather than left to become a link error about a symbol nobody can act on.
  find_path(GLSLANG_INCLUDE_DIR glslang/Public/ShaderLang.h
            PATHS /usr/include /usr/include/i386-linux-gnu
            NO_CMAKE_FIND_ROOT_PATH)
  find_library(GLSLANG_LIBRARY NAMES glslang
               PATHS /usr/lib/i386-linux-gnu /usr/lib32 /usr/lib
               NO_CMAKE_FIND_ROOT_PATH)

  if(GLSLANG_INCLUDE_DIR AND GLSLANG_LIBRARY)
    message(STATUS "libglslang ${GLSLANG_LIBRARY} -- the shader runtime will compile GLSL at run time")
    add_library(shaderc_rt STATIC src/shader/glslang_posix.cpp)
    target_include_directories(shaderc_rt PUBLIC src ${GLSLANG_INCLUDE_DIR})
    # libglslang.a is a static archive and its SPIR-V emitter calls into SPIRV-Tools, so those come with
    # it. Found by name rather than hard-coded, because a machine with a shared libglslang would not have
    # them at all -- and a static archive that needs a library nobody listed fails with undefined references
    # about spvContextCreate, which says nothing about where they come from.
    # Three more archives, and the order matters because these are all static. libglslang.a's emitter calls
    # spvtools::Optimizer (which lives in the -opt archive, not the plain one), which calls into libSPIRV.a,
    # and the default resource table lives in a fourth archive of its own. A missing one shows up as an
    # undefined reference to a symbol nobody can place, which is why each is named in a comment.
    set(GLSLANG_EXTRA_LIBS)
    foreach(lib SPIRV SPIRV-Tools-opt SPIRV-Tools glslang-default-resource-limits)
      find_library(GLSLANG_LIB_${lib} NAMES ${lib} PATHS /usr/lib/i386-linux-gnu /usr/lib32 /usr/lib
                   NO_CMAKE_FIND_ROOT_PATH)
      if(GLSLANG_LIB_${lib})
        list(APPEND GLSLANG_EXTRA_LIBS ${GLSLANG_LIB_${lib}})
      else()
        message(WARNING "  ${lib} not found -- linking libglslang will probably fail on undefined references")
      endif()
    endforeach()
    # libSPIRV.a before SPIRV-Tools-opt before libglslang.a: the archives are searched in the order given.
    target_link_libraries(shaderc_rt PUBLIC ${GLSLANG_LIBRARY} ${GLSLANG_EXTRA_LIBS})
    target_compile_options(shaderc_rt PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(blackcore PUBLIC shaderc_rt)
    set(HAVE_GLSLANG ON)
  else()
    message(WARNING
      "libglslang for i386 was not found, so the shader runtime is disabled and A5 cannot build.\n"
      "  Install it with:  sudo apt install glslang-dev:i386\n"
      "  The x86-64 package does not help: this loader is a 32-bit process.\n"
      "  Everything else here builds and tests without it.")
    set(HAVE_GLSLANG OFF)
  endif()

  # Shaders are compiled at build time and embedded, so the loader ships with its SPIR-V inside it and needs
  # nothing installed to run. glslangValidator comes from glslang-tools; without it the graphics suite is
  # skipped rather than failing the build, because everything else here does not need a shader.
  find_program(GLSLANG_VALIDATOR glslangValidator)
  # find_program caches, so an upgrade that *removes* the binary leaves the path sitting in the cache and
  # the build goes on believing in a file that is no longer there. Re-checking the file is one line and turns
  # a confusing "no such file or directory" from deep inside a custom command into the host tool being used.
  if(GLSLANG_VALIDATOR AND NOT EXISTS "${GLSLANG_VALIDATOR}")
    message(STATUS "glslangValidator was uninstalled since the last configure -- not using the cached path")
    unset(GLSLANG_VALIDATOR CACHE)
  endif()
  # The same compiler the shader runtime links, as a build tool. Preferred when glslangValidator is absent,
  # which is what happens on a machine that installed glslang-dev:i386: that package conflicts with the
  # amd64 glslang-tools, so apt removes the binary. Keeping the build dependent on it would mean installing
  # A5's dependency silently breaks the A0 gate, which links the shaders generated here.
  set(GLSLANG_HOST_TOOL)
  if(HAVE_GLSLANG)
    add_executable(glslc_host tools/glslc_host.cpp)
    target_link_libraries(glslc_host PRIVATE shaderc_rt)
    target_include_directories(glslc_host PRIVATE ${GLSLANG_INCLUDE_DIR})
    set(GLSLANG_HOST_TOOL $<TARGET_FILE:glslc_host>)
  endif()
  set(SPV_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/shaders)
  if(GLSLANG_VALIDATOR OR GLSLANG_HOST_TOOL)
    file(MAKE_DIRECTORY ${SPV_DIR})
    # SPIR-V 1.0 / Vulkan 1.0 is the target, not the newest the compiler can emit: llvmpipe's shader JIT on
    # this machine (Mesa 24.x, 32-bit) segfaults inside vkCreateGraphicsPipelines on SPIR-V from a
    # vulkan1.3 target, and nothing in the port needs an instruction newer than 1.0. A shader that works on
    # the software rasteriser works everywhere.
    set(SPIRV_TARGET_ENV vulkan1.0)
    # GLSL -> SPIR-V -> a C header of uint32 words. Both steps are build steps with declared outputs, so
    # ninja knows the header is older than the shader and rebuilds it; reading the SPIR-V during configure
    # instead would need the .spv to exist in a clean tree, which it never does.
    # One custom command per shader *file*, because the stages are not a fixed pair: the textured probe's
    # fragment shader is a third file, and it exists separately precisely because it needs descriptors.
    # One custom command per shader file, with the C symbol spelled out next to it. Doing this by string
    # surgery on the file name produced "kProbevert" the first time, and the compiler caught it.
    set(SPV_HEADERS)
    # One command shape for both compilers, chosen here rather than in the loop: the validator takes the
    # source and -o, the host tool takes the stage and the target env. Spelling that out per shader is how a
    # stage name ends up in the wrong slot of the wrong command.
    if(GLSLANG_VALIDATOR)
      set(GLSLANG_COMPILE ${GLSLANG_VALIDATOR})
      message(STATUS "shader compilation: glslangValidator at ${GLSLANG_VALIDATOR}")
    else()
      set(GLSLANG_COMPILE ${GLSLANG_HOST_TOOL})
      message(STATUS "shader compilation: glslc_host (glslangValidator is not installed)")
    endif()
    set(SPV_LIST "probe.vert|kProbeVert" "probe.frag|kProbeFrag" "probe_tex.frag|kProbeTexFrag")
    foreach(entry ${SPV_LIST})
      string(REPLACE "|" ";" parts ${entry})
      list(GET parts 0 stem)
      list(GET parts 1 name)
      string(REPLACE "." "_" sym ${stem})
      set(spv ${SPV_DIR}/${stem}.spv)
      set(hdr ${SPV_DIR}/${sym}_spirv.h)
      add_custom_command(
        OUTPUT ${hdr}
        COMMAND ${GLSLANG_COMPILE} -V --target-env ${SPIRV_TARGET_ENV} -o ${spv}
                ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${stem}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${spv} -DOUTPUT=${hdr} -DNAME=${name}Spv
                -P ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gen_spirv_header.cmake
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${stem}
                ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gen_spirv_header.cmake
        COMMENT "GLSL to SPIR-V to header: ${stem}")
      list(APPEND SPV_HEADERS ${hdr})
    endforeach()
    add_custom_target(blackshaders DEPENDS ${SPV_HEADERS})
    add_library(blackshaders_headers INTERFACE)
    target_include_directories(blackshaders_headers INTERFACE ${SPV_DIR})
  else()
    message(WARNING "no GLSL to SPIR-V compiler found (neither glslangValidator nor glslc_host) -- the "
                    "headless graphics suite and the A0 gate will be skipped")
  endif()

# ---- audio decoder: FFmpeg libavcodec (i386) for WMA/XMV ----
# The game uses WMA for audio and XMV for video. Both decode via libavcodec.
# We need the i386 dev package: libavcodec-dev:i386 (also pulls libavformat, libavutil, libswresample, libswscale).
find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h
          PATHS /usr/include /usr/include/i386-linux-gnu
          NO_CMAKE_FIND_ROOT_PATH)
find_library(AVCODEC_LIB avcodec PATHS /usr/lib/i386-linux-gnu /usr/lib32 /usr/lib NO_CMAKE_FIND_ROOT_PATH)
find_library(AVFORMAT_LIB avformat PATHS /usr/lib/i386-linux-gnu /usr/lib32 /usr/lib NO_CMAKE_FIND_ROOT_PATH)
find_library(AVUTIL_LIB avutil PATHS /usr/lib/i386-linux-gnu /usr/lib32 /usr/lib NO_CMAKE_FIND_ROOT_PATH)
find_library(SWRESAMPLE_LIB swresample PATHS /usr/lib/i386-linux-gnu /usr/lib32 /usr/lib NO_CMAKE_FIND_ROOT_PATH)
find_library(SWSCALE_LIB swscale PATHS /usr/lib/i386-linux-gnu /usr/lib32 /usr/lib NO_CMAKE_FIND_ROOT_PATH)

if(AVCODEC_INCLUDE_DIR AND AVCODEC_LIB AND AVFORMAT_LIB AND AVUTIL_LIB AND SWRESAMPLE_LIB AND SWSCALE_LIB)
  message(STATUS "FFmpeg libavcodec ${AVCODEC_LIB} -- audio/video decoder will link")
  set(FFMPEG_LIBS ${AVCODEC_LIB} ${AVFORMAT_LIB} ${AVUTIL_LIB} ${SWRESAMPLE_LIB} ${SWSCALE_LIB})
  set(HAVE_FFMPEG ON)
else()
  message(WARNING
    "libavcodec-dev:i386 was not found, so the audio/video decoder (A9) is disabled.\n"
    "  Install it with:  sudo apt install libavcodec-dev:i386\n"
    "  The x86-64 package does not help: this loader is a 32-bit process.\n"
    "  Everything else here builds and tests without it.")
  set(HAVE_FFMPEG OFF)
endif()

# Link FFmpeg libraries to blackcore if available
if(HAVE_FFMPEG)
  target_include_directories(blackcore PUBLIC ${AVCODEC_INCLUDE_DIR})
  # Grouped because libavformat and libavcodec reference each other (av_parser_* lives in avcodec and is
  # called by avformat's demuxer), and a plain archive order fails to resolve one of the two directions.
  # Grouping is harmless for the shared libraries a distro install ships; it matters for a static build.
  target_link_libraries(blackcore PUBLIC "-Wl,--start-group" ${FFMPEG_LIBS} "-Wl,--end-group" m)
  target_compile_definitions(blackcore PUBLIC HAVE_FFMPEG=1)
  # The stream layer is only meaningful on top of a real decoder, so it is built only here. Added here
  # rather than in the source list above because HAVE_FFMPEG is not decided until line ~267, after
  # add_library(blackcore). Built unconditionally it was unreachable code that emitted seven
  # -Wunused-function warnings and implied an A9 decoder that does not exist.
  target_sources(blackcore PRIVATE src/audio/stream_decode_posix.cpp)
endif()
  # The loader itself. There is no stub and no DLL on Linux: the program maps the image over its own
  # address space, so main() is the whole entry point (src/platform/posix/boot.h has the sequence).
  add_executable(blacklinux src/platform/posix/main.cpp src/platform/posix/doctor.cpp src/platform/posix/window.cpp)
  target_link_libraries(blacklinux PRIVATE blackcore)
  # Platform layer tests. These need no XBE: they exercise the TEB anchor, the fs->gs patcher and the
  # SEH chain directly.
  enable_testing()

  # A9: the decoder must actually decode. The test encodes a WMAv2 stream with libavcodec, feeds the
  # packets through this repository's packet-driven wrapper, and checks the sine waves that come back.
  # It is registered only when FFmpeg is present, because without it the wrapper is an honest stub and
  # there is nothing to drive. The XMV half additionally decodes the first frames of a real game video
  # when game/videos/ exists; that part skips itself when the dataset is absent.
  if(HAVE_FFMPEG)
    add_executable(test_decoder tests/test_decoder.cpp)
    target_link_libraries(test_decoder PRIVATE blackcore)
    target_compile_definitions(test_decoder PRIVATE
                               BLACKLINUX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
    add_test(NAME decoder COMMAND test_decoder)
  endif()

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
  # The library-section hook installer: every D3D/DSOUND/XPP function patched at its entry, checked
  # against a synthetic image so the sizes and addresses come from the real tables and nothing else.
  add_executable(test_d3d_hooks tests/test_d3d_hooks.cpp)
  target_link_libraries(test_d3d_hooks PRIVATE blackcore)
  add_test(NAME d3d_hooks COMMAND test_d3d_hooks)
  # The Xbox D3D8 ABI, with every size and offset derived a second time in the test itself. No game image
  # needed: this is the layer whose mistakes are invisible until the real thing runs.
  add_executable(test_xd3d8 tests/test_xd3d8.cpp)
  target_link_libraries(test_xd3d8 PRIVATE blackcore)
  add_test(NAME xd3d8 COMMAND test_xd3d8)

  # The HLE thunk builder: an LTCG call site with register arguments, forwarded to a __stdcall
  # implementation, with the caller's stack and registers measured afterwards. This is where the four
  # stdcall-shaped bugs of this port were found, so it is checked from both ends.
  add_executable(test_thunk tests/test_thunk_posix.cpp)
  target_link_libraries(test_thunk PRIVATE blackcore)
  add_test(NAME thunk COMMAND test_thunk)

  # The HLE state functions: every table row builds a callable thunk, and calling one moves exactly the
  # bytes the game would read. A function that writes a shadow the game cannot see is not state.
  add_executable(test_hle_device tests/test_hle_device.cpp)
  target_link_libraries(test_hle_device PRIVATE blackcore)
  add_test(NAME hle_device COMMAND test_hle_device)

  # The vertex-format table: pure arithmetic over the console's declaration, so it needs no device. This is
  # the part of A4 that can be finished without a GPU that submits; the rest of the draw path waits on A5.
  add_executable(test_vertex_posix tests/test_vertex_posix.cpp)
  target_link_libraries(test_vertex_posix PRIVATE blackcore)
  add_test(NAME vertex_posix COMMAND test_vertex_posix)

  # The shader runtime needs libglslang, so this one is only registered when it was found. A missing test is
  # better than a failing one here: the wrapper is written against a library whose API moved, and a test
  # that fails to build because of it would read as a defect in the port rather than a missing package.
  if(HAVE_GLSLANG)
    add_executable(test_shader_rt tests/test_shader_rt.cpp)
    target_link_libraries(test_shader_rt PRIVATE blackcore shaderc_rt)
    add_test(NAME shader_rt COMMAND test_shader_rt)

    # The closed loop: hand-written NV2A microcode, this repository's translation to GLSL, libglslang's
    # compilation, and a check that a real SPIR-V module came out. It needs no GPU and no image, which is the
    # point -- before libglslang was installed there was no way to verify a translation at all.
    add_executable(test_nv2a_vsh tests/test_nv2a_vsh.cpp)
    target_link_libraries(test_nv2a_vsh PRIVATE blackcore shaderc_rt)
    add_test(NAME nv2a_vsh COMMAND test_nv2a_vsh)

    add_executable(test_nv2a_psh tests/test_nv2a_psh.cpp)
    target_link_libraries(test_nv2a_psh PRIVATE blackcore shaderc_rt)
    add_test(NAME nv2a_psh COMMAND test_nv2a_psh)

    # The whole chain, ending in something the driver accepts. Labelled `gpu` because it needs a device; it
    # uses llvmpipe, so it is skipped by the 77 convention on a machine with no Vulkan at all.
    add_executable(test_material tests/test_material.cpp)
    target_link_libraries(test_material PRIVATE blackcore shaderc_rt)
    add_test(NAME material COMMAND test_material)
    set_tests_properties(material PROPERTIES LABELS "gpu" SKIP_RETURN_CODE 77)

    add_executable(test_managed_buffer tests/test_managed_buffer.cpp)
    target_link_libraries(test_managed_buffer PRIVATE blackcore)
    add_test(NAME managed_buffer COMMAND test_managed_buffer)
    set_tests_properties(managed_buffer PROPERTIES LABELS "gpu" SKIP_RETURN_CODE 77)

    add_executable(test_resource tests/test_resource.cpp)
    target_link_libraries(test_resource PRIVATE blackcore)
    add_test(NAME resource COMMAND test_resource)

    add_executable(test_draw_posix tests/test_draw_posix.cpp)
    target_link_libraries(test_draw_posix PRIVATE blackcore)
    add_test(NAME draw_posix COMMAND test_draw_posix)

    add_executable(test_device_gpu tests/test_device_gpu.cpp)
    target_link_libraries(test_device_gpu PRIVATE blackcore)
    add_test(NAME device_gpu COMMAND test_device_gpu)
    set_tests_properties(device_gpu PROPERTIES LABELS "gpu" SKIP_RETURN_CODE 77)
  else()
    message(STATUS "libglslang absent -- the shader runtime test is not registered")
  endif()

  # DPCs and kernel timers: the last six kernel ordinals, checked against what the game can observe.
  add_executable(test_kdpc tests/test_kdpc.cpp)
  target_link_libraries(test_kdpc PRIVATE blackcore)
  add_test(NAME kdpc COMMAND test_kdpc)
  # The graphics device, headless: a real device, a real pipeline, real shaders, and a PNG written from the
  # result. This is the gate that says the Vulkan path works at all on this machine -- with no window and no
  # XBE, which is the only way it can be run before the image arrives.
  # The same condition that built the shaders above. This guard used to test a variable that no longer
  # exists, which dropped test_gfx, test_vk32 and vk_probe32 from the build without a word -- two tests
  # simply were not there, and a suite count is the kind of number nobody checks twice.
  if(GLSLANG_VALIDATOR OR GLSLANG_HOST_TOOL)
    add_executable(test_gfx tests/test_gfx.cpp)
    target_link_libraries(test_gfx PRIVATE blackcore blackshaders_headers)
    add_dependencies(test_gfx blackshaders)
    add_test(NAME gfx COMMAND test_gfx)

    # A0, the Vulkan 32-bit gate: every physical device, one forked child each, and the answer as an exit
    # code -- 0 drew, 1 did not, 77 there is no hardware GPU to ask. Labelled `gpu` with 77 as the skip code
    # so a machine without a GPU records "skipped" instead of failing; a software rasteriser must never be
    # this gate, because it accepts work and then draws nothing.
    add_executable(test_vk32 tests/probe_vk32.cpp)
    target_link_libraries(test_vk32 PRIVATE blackcore blackshaders_headers)
    add_dependencies(test_vk32 blackshaders)
    add_test(NAME vk32 COMMAND test_vk32)
    set_tests_properties(vk32 PROPERTIES LABELS gpu SKIP_RETURN_CODE 77 TIMEOUT 300)

    # A diagnostic, not a test: it asks the 32-bit Intel driver three questions (empty submit on 1.0, empty
    # submit on 1.3, submit with dynamic rendering) to find out whether the crash A0 found is the driver or
    # this port asking for Vulkan 1.3. The answer decides whether A4 can stay in-process at all.
    add_executable(vk_intel_diag tools/vk_intel_diag.cpp)
    target_link_libraries(vk_intel_diag PRIVATE Vulkan::Vulkan)

    # The same source as a standalone probe, to run by hand on a GPU this repository cannot see. That is how
    # the other rows of the driver table in NOTES.md get filled in.
    add_executable(vk_probe32 tests/probe_vk32.cpp)
    target_link_libraries(vk_probe32 PRIVATE blackcore blackshaders_headers)
    add_dependencies(vk_probe32 blackshaders)
  endif()
  # How a run ends and what the exit code says: halt, bugcheck, --strict, and the ordinary case. Each
  # scenario forks, because the exit request is process-wide and one-shot by design.
  add_executable(test_exit tests/test_exit.cpp)
  target_link_libraries(test_exit PRIVATE blackcore)
  add_test(NAME exit COMMAND test_exit)
  # The window and the present path: a real SDL2 window, a real surface, a real swapchain, and frames that
  # are acquired, cleared, submitted and presented -- then the same again after a resize, with the pixels
  # of the presented drawable read back where the surface allows it. Labelled `gpu` with 77 as the skip
  # code because every one of those needs a display and a Vulkan device, and a machine without either has
  # to record "skipped" rather than fail. Needs no XBE, which is the point: this is the path that can be
  # finished before the image arrives.
  # Not a test: writes the same synthetic image to a file, so the program itself can be run against it.
  add_executable(fixture_dump tests/fixture_dump.cpp)
  target_link_libraries(fixture_dump PRIVATE blackcore)
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
  # Threads: what a worker finds in its own KPCR block -- KPRCB, IRQL, KTHREAD, TLS slots -- plus
  # suspend/resume, priorities, and the entry-point shapes the Xbox uses.
  add_executable(test_kthread tests/test_kthread.cpp)
  target_link_libraries(test_kthread PRIVATE blackcore)
  add_test(NAME kthread COMMAND test_kthread)
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
endif()
