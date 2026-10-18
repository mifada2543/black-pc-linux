# BLACK (Xbox, NTSC-U) native PC port

Read `NOTES.md` (findings, build facts, environment) and `docs/ROADMAP.md` (7-phase plan) first. This file is the short version plus rules.

## What this project is

A **32-bit i386 Linux program** that loads the user's own `game/default.xbe` and runs the original x86
code natively, with the Xbox kernel emulated in-process (HLE, like Cxbx-Reloaded). The process must be
32-bit: long mode cannot execute the game's x86-32 code and the image bakes absolute addresses into
itself. D3D8, DSound and XAPI are being replaced by Linux code — Vulkan, then SDL2 — but **that layer does
not exist yet**, so the game cannot be played.

This is a fork of the Windows port by Spark198rus. **The project is Linux-only now.** The original
Windows implementation is archived in `loader/windows/`, which `.gitignore` excludes from git: it is the
*invoice* for this code — the only place certain non-obvious workarounds are explained — not a second
build, and nothing in `loader/CMakeLists.txt` refers to it. Read it for the "why" before porting anything;
never port the mechanism without the reason.

## State (2026-10-18)

Linux port, i386, in `loader/src` (44 files) with 17 test suites and 930 checks, all green without the
game. Working: XBE parse, image map at `0x10000`, section copy, `%fs:` → `%gs:` rewriting (every
displacement, including those inside the per-thread KPCR block), all **115** kernel imports resolved with
**no stubs left** (102 implemented + 13 data exports), mini-SEH, the Xbox filesystem/memory/sync/thread/
DPC layers, and the entry point run on a real thread created through the thread layer. Entry hooks for the
XBE's D3D/DSOUND/XPP sections are installed (819 functions; 8 too short for a jump trap instead, and the
signal bridge recognises them). A headless Vulkan device exists (`loader/src/gfx`) and is tested as far as
this machine allows — its 32-bit drivers rasterise nothing, which is measured and reported, not assumed.

**Not built:** D3D8 (so no drawing and no playable game), SDL2 host window/input/audio, and anything that
needs the real image. `.agents/plan.md` holds the plan, the phase gates, the risk register and the open
blockers; `docs/CARA-PASANG-GAME.md` is the Indonesian install/run guide. Build, run and the source layout
are in `loader/README.md`; the archived port's own README is `loader/windows/README.md`.

## Hard rules

- **Never commit, upload or share**: `game/`, `*.xbe`, `*.iso`, `work/` (decompiler output), `ghidra/` (analysed binary), `notes/ghidra-strings.txt`.
  They are in `.gitignore`; check it still is before any `git add`. No Microsoft XDK headers/libs/code in the repo.
- Do not run `git commit`/`push` or `git init` without the user asking. Publishing is the user's call.
- The user supplies the dump; the loader reads `default.xbe` and assets from a path at runtime. Nothing game-derived is embedded in the loader.
- Cxbx-Reloaded and xemu are GPL: read them as reference; do not paste their code into this project without the user deciding the license first.
- Everything under `tools/` third-party clones and `tools/bin/` is local only (also git-ignored). macOS binaries in `tools/bin/` do not run on Windows.
- **`loader/windows/` is the archived Windows port and is deliberately not in git** (`.gitignore`). Do not
  "fix" that by committing it, do not add Linux build rules for it, and do not port a mechanism from it
  without porting the comment that explains it. It stays buildable on Windows with MSVC (`loader/windows/CMakeLists.txt`);
  two shared files stay outside it on purpose: `loader/src/sha1.*` (one SHA-1) and `loader/src/*_functions.inc`
  (section tables, which are facts about the game build).

## Known facts that shape the design (details and addresses in NOTES.md)

- Image base `0x10000`, image size `0x577a40` (occupies `0x10000-0x587a40`), entry `0x25a3f`, 115 kernel imports (`notes/phase1-xbe-report.txt`).
- Reserve the XBE's address range **at link time** if possible (a linker-reserved section at that base), not only with `VirtualAlloc` at startup:
  Windows may already have put stacks/heaps low. Also plan for Xbox contiguous-memory/physical aliases (`0x80000000`, `0xF0000000`);
  the game imports `MmAllocateContiguousMemoryEx`, `MmClaimGpuInstanceMemory`, `MmGetPhysicalAddress`. Cxbx-Reloaded's approach is the reference.
- D3D is statically linked, LTCG-compiled (`D3D8LTCG`), and talks to hardware, so it is replaced **per function** (hook the entry address with a jump).
  Many LTCG functions take args in registers (XbSymbolDatabase names encode them: `_4__LTCG_eax1` etc.), so hooks need naked asm thunks.
- The game calls 70 distinct D3D functions (`notes/ghidra-summary.txt`); 9 of them are unnamed (listed in NOTES.md).
- The game **writes into the D3D library's data** (render-state shadow array `0x27c1xx-0x27c2xx`, dirty flags `0x27c000`): keep that memory in place, read it at draw time.
- Only 3 callers use raw push buffers (`0x2bac0`, `0x37e90`, `0x3e1b0`), always `NV097_SET_BEGIN_END` (`0x17fc`) + `NV097_INLINE_ARRAY` (`0x1818`).
  A mini interpreter for those 2 methods is enough.
- Audio needs more than DSound: `WMADEC` (WMA decoder) and `DOLBY` sections are linked in, videos are `.xmv`. None of those have symbols.
- Ghidra's decompiler output is analysis input, not compilable source.

## Tools on this side

`python tools/xbeinfo.py game/default.xbe` prints the XBE structure. Ghidra project: import `ghidra/default.xbe.gzf` (File > Import File);
it already contains all analysis and labels. `notes/functions.csv` lists every function (address, name, size, section, refs).
`ghidra_scripts/*.java` are headless-capable report/decompile scripts (`BlackSummary`, `DumpDecomp`, `ExportForPc`).

## Working style

Gate each phase with a concrete check and say plainly when a gate is not met. Log every unimplemented kernel/D3D call on first use rather than guessing.
Keep `NOTES.md` updated with findings (addresses, structures) so the next session does not rediscover them.
