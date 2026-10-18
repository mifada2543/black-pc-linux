# BLACK — Linux port

A native Linux port of **BLACK** (Criterion Games, 2006), made from the original Xbox version. The
game's own x86-32 code runs unmodified: the loader maps `default.xbe` over its own address space and
calls its entry point, with the Xbox kernel emulated in-process.

This is a fork of [Spark198rus/black-pc](https://github.com/Spark198rus/black-pc), which was a Windows
port. **This repository is the Linux port and nothing else**: `loader/src` builds a 32-bit i386 program,
and the original Windows implementation — the record of which workarounds exist and why — is archived
locally in `loader/windows/` and deliberately kept out of git. See `loader/README.md`.

**You need your own copy of the game.** This repository contains no game code or data.

## Status

Under development, and honest about where it stands:

- **Working, and tested without the game.** XBE parse, image map, section copy, `%fs:` → `%gs:`
  rewriting, all **115** kernel imports resolved (no stubs left), the entry point executed on a real
  thread with its own KTHREAD, KPRCB, TLS block and DPC worker; the Xbox filesystem, memory allocator,
  sync/IRQL, timers and DPCs; mini-SEH, so the game's own exception handlers run.
- **Not there yet.** There is no D3D8 layer, so the game cannot draw and is **not playable**. The
  graphics work currently reaches a headless Vulkan device (targets, depth, textures, samplers,
  pipelines built from SPIR-V, readback) — see `loader/src/gfx/`. Entry hooks for the XBE's D3D/DSOUND/XPP
  sections are installed, so the first library call the game makes is reported by name rather than
  faulting inside NV2A code.
- **Blocked on the game image.** Everything that can be verified without `default.xbe` is verified;
  17 test suites, 930 checks. What cannot be checked until the image is available is listed in
  `.agents/plan.md` under the Phase 1 gate.

## Requirements

- A 32-bit-capable Linux toolchain and the 32-bit Vulkan loader (see **Setting up**).
- The NTSC-U (North America) Xbox disc of BLACK, dumped by you. Other regions and builds have different
  code addresses and are not supported.

## Setting up

1. **The game.** Dump your NTSC-U version to an ISO and extract it with
   [extract-xiso](https://github.com/XboxDev/extract-xiso) (`extract-xiso -x Black.iso`). Rename the
   extracted folder to `game/`.
2. **The 32-bit toolchain.** The loader *must* be a 32-bit process: long mode cannot execute the game's
   x86-32 code, and the game bakes absolute addresses into itself.

   ```
   sudo apt install gcc-multilib g++-multilib libc6-dev-i386 \
                    libvulkan-dev:i386 libvulkan1:i386 mesa-vulkan-drivers:i386 \
                    glslang-tools
   ```

   Optional, for the window/input/audio host layer that comes later: `libsdl2-dev:i386`.
   (`sudo` is yours to run — the port never installs anything itself.)

3. **Build and test.**

   ```
   cd loader
   cmake --preset linux
   cmake --build --preset linux
   ctest --preset linux
   ```

4. **Run.** `docs/CARA-PASANG-GAME.md` is the step-by-step guide (Indonesian), including what to expect
   in the log and what each exit code means.

   ```
   build/linux/blacklinux --xbe game/default.xbe --probe --timeout 15 --trace
   ```

## How it works

One process, no stub and no DLL. `main()` maps the image at `0x10000` with a single `mmap`, copies its
sections, and runs the entry point on a thread the loader created itself — so the game's first
instruction already finds a KTHREAD behind `fs:[0x20]`. Two things are genuinely different from the
Windows port and worth knowing:

- **`%fs` cannot be aimed on i386 Linux.** Every `fs:[disp]` in the image is rewritten to `gs:[K+disp]`,
  where `K` is measured at run time against glibc's TCB, which sits in an ordinary `__thread` block
  shaped like the console's KPCR. The displacement of every single one of them is patched, including the
  ones inside the block itself.
- **The Xbox libraries are linked into the game**, so there is no import table to redirect. Each function
  in the XBE's D3D/DSOUND/XPP sections is patched at its entry: with a replacement where one exists, and
  otherwise with a thunk that names the function, reports it, and stops the run.

Design notes and findings are in `NOTES.md`. The plan, phase gates and risk register are in
`.agents/plan.md`. `notes/` holds analysis results used to build the address tables: function addresses
and symbol names, the XBE header report and checksums of the disc's files — no game code.

## The archived Windows port

The original playable port (`black.exe` + `blackrt.dll`, D3D9, XAudio2, XInput) is **not in this
repository**. It lives in `loader/windows/`, which `.gitignore` excludes: it is the invoice for the Linux
code — the only place the reasoning behind certain non-obvious workarounds is written down — not a
second build. A clone without it builds and tests the Linux loader unchanged. `loader/windows/README.md`
explains what is in it and how to build it if you have the archive.

## Legal

This project is not affiliated with or endorsed by Criterion Games or Electronic Arts. BLACK is a
trademark of Electronic Arts Inc.; Xbox is a trademark of Microsoft Corporation. No code or data from the
game and no code from the Microsoft Xbox Development Kit is included. You need your own legally obtained
copy of the game.

The port is free software under the GNU General Public License, version 2 or (at your option) any later
version. See `LICENSE`.

## Thanks

[Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded) and [xemu](https://xemu.app) served as
references for how the Xbox kernel and the NV2A behave (no code was copied).
[XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase) identified the Xbox library
functions in the game, [Ghidra](https://ghidra-sre.org) with [ghidra-xbe](https://github.com/XboxDev/ghidra-xbe)
was used for the analysis, and the [nxdk](https://github.com/XboxDev/nxdk) headers document the kernel's
types.

Original Windows port by Spark198rus; the Linux port continues from it.