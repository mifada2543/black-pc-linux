# BLACK loader (Linux)

Runs the original `default.xbe` natively as a **32-bit i386 process on Linux**, with the Xbox kernel
emulated in-process. The game's own x86-32 code runs unmodified: the loader maps the XBE image over its
own address space and calls its entry point on a real thread.

**You need your own copy of the game.** This repository contains no game code or data — see
`../docs/CARA-PASANG-GAME.md` (Indonesian) for the install and run guide.

## Status

The loader boots a real XBE-shaped image end to end: parse, map, copy sections, rewrite `%fs:` → `%gs:`,
resolve all **115** kernel imports, run the entry point on a thread that already has a KTHREAD, KPRCB,
TLS block and DPC worker behind it. **There is no D3D8 layer yet**, so the game cannot draw and cannot be
played — the graphics work stops at a headless Vulkan device (`src/gfx/`).

Everything up to that point is covered by 17 test suites (930 checks) that need no game at all.

## Build

Needs a 32-bit toolchain, SDL2 headers (not yet used by the loader itself) and the 32-bit Vulkan loader:

```
sudo apt install gcc-multilib g++-multilib libc6-dev-i386 libvulkan-dev:i386 libvulkan1:i386 \
                 mesa-vulkan-drivers:i386 libglslang-dev glslang-tools
```

(`sudo` is yours to run — the port never installs anything itself.) See `../docs/CARA-PASANG-GAME.md`
for the full list, including the optional SDL2 packages.

```
cmake --preset linux
cmake --build --preset linux
ctest --preset linux
```

The process **must** be 32-bit: long mode cannot execute the XBE's x86-32 code, and the game bakes
absolute addresses into itself. The `linux` preset passes `-m32`, and `CMakeLists.txt` refuses a
64-bit configure rather than failing later in a confusing way.

Output: `build/linux/blacklinux`.

## Run

```
build/linux/blacklinux --xbe game/default.xbe --probe --timeout 15 --trace
build/linux/blacklinux --xbe game/default.xbe --info      # header, sections and imports, then exit
```

| Option | |
|---|---|
| `--xbe <path>` | the XBE to run (default `game/default.xbe`) |
| `--info` | print the image and exit, without mapping it |
| `--probe --timeout <s>` | enter the entry point, wait, report where the game is, exit |
| `--log <path>` | log file (default `blacklinux.log`) |
| `--hdd <path>` | writable folder for `\Device\Harddisk0` (default `$XDG_DATA_HOME/blacklinux`) |
| `--trace` | detail lines and every kernel-stub call on the console as well as in the log |
| `--strict` | an unimplemented kernel call ends the run (exit code 2) instead of being logged |
| `--version`, `--help` | |

Without `--probe` the program waits for the entry point to return — which the real game never does, so
`--probe` + `--timeout` is how it is run today.

Exit codes: `0` normal (including `HalReturnToFirmware(0)` = halt), `1` a fatal error the game reported or
a failure to start, `2` bad arguments or `--strict` stopped the run, `4` bugcheck. Tested by
`tests/test_exit.cpp`, so they are not accidental.

## Layout

Everything here is the Linux port. The archived Windows port is in `windows/`, which is deliberately
**not in git** (see `.gitignore`): it is the invoice for this code — the record of *why* each workaround
exists — not a second build. Nothing in `CMakeLists.txt` refers to it, and a clone without it builds and
tests exactly as before.

| Path | What |
|---|---|
| `src/platform/posix/` | the platform layer: image mapping, the `%fs`→`%gs` patcher, the per-thread KPCR/TEB block, mini-SEH, the XBE parser, the Xbox file layer, the boot sequence |
| `src/kernel/*_posix.*` | the HLE Xbox kernel: the 115-ordinal table, memory, files, sync/IRQL, `Rtl*`, threads, DPCs and timers |
| `src/gfx/` | the Vulkan device layer (targets, depth, textures, samplers, pipelines from embedded SPIR-V, readback) and a PNG writer |
| `src/d3d/d3d_hooks_posix.*` | entry hooks for the XBE's library sections (D3D/DSOUND/XPP): every function patched at its entry, unimplemented ones report themselves by name |
| `shaders/` | GLSL compiled to SPIR-V at build time by `glslangValidator` |
| `tests/` | 17 suites; `tests/fixture_gen/` assembles the synthetic XBE the boot path is tested against |
| `src/sha1.*`, `src/*_functions.inc` | shared with the archived port on purpose: one SHA-1 implementation, and section tables that are facts about the game build rather than about either port |
| `windows/` | the archived Windows port — reference only, not in git, not built |

## Tests

```
ctest --preset linux                       # everything
build/linux/test_boot                      # the boot path against a synthetic image, with details
build/linux/test_kdpc                      # what a game observes from timers and DPCs
build/linux/test_d3d_hooks                 # the library-section hook installer
```

`tests/fixture_dump /tmp/boot.xbe` writes the same synthetic image to a file, so the program itself can
be run against it by hand.

Design notes and findings are in `../NOTES.md`; the plan, the phase gates and the risk register are in
`../.agents/plan.md`.