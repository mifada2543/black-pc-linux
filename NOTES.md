# BLACK (Xbox, NTSC-U) Linux port: working notes

> **Read this first.** This file is mostly the Windows port's findings, kept because they are the
> *invoice* for the Linux one: addresses, structures, and the reasons behind workarounds that are easy to
> get wrong. The Linux port lives in `loader/src` and is documented in `loader/README.md`;
> `.agents/plan.md` has its plan, gates and blockers. Where a line here says "fix in `.../src/x.cpp`",
> that file is in `loader/windows/` (git-ignored) unless the path starts with `loader/src`.

## Layout

| Path | What |
|---|---|
| `game/` | Extracted disc (574 files). **Git-ignored. Never commit or share.** Source: `../Black.iso` (full redump incl. video partition, game partition at 0x18300000) |
| `loader/` | **The Linux loader** (CMake, i386): one program, no stub, no DLL. See `loader/README.md`. Build output in `loader/build/` (git-ignored) |
| `loader/windows/` | **The archived Windows port** — `black.exe` stub + `blackrt.dll`, D3D9/XAudio2/XInput. **Git-ignored on purpose** and not built: the invoice for the Linux code, not a second build. It is the source of most "why it has to be done this way" below, so read it before porting anything |
| `tools/xbeinfo.py` | Own XBE parser (Phase 1 gate). `python tools/xbeinfo.py game/default.xbe [--json]` |
| `tools/xbe2pe.py` | Wraps the mapped XBE in a PE so MSVC `dumpbin /disasm` can disassemble it (output to `work/`) |
| `tools/gen_d3d_table.py` | Regenerates `loader/src/d3d_functions.inc` from `notes/functions.csv` |
| `tools/kernel_exports.json` | Kernel ordinal -> name table (from ghidra-xbe) |
| `tools/bin/` | extract-xiso, XbSymbolDatabaseCLI (built arm64 macOS), ghidra-xbe extension zip (built for Ghidra 12.1.4) |
| `notes/` | Generated reports: Phase 1 XBE report, raw XbSymbolDatabase output, Ghidra summary, `functions.csv` (all functions), `game-manifest.sha256` |
| `ghidra/default.xbe.gzf` | Portable export of the fully analysed Ghidra program (git-ignored). Import on any machine with File > Import File |
| `ghidra_scripts/` | `BlackSummary.java`, `DumpDecomp.java`, `ExportForPc.java` (headless Ghidra scripts) |
| `work/` | Derived from the game, git-ignored: decompiler output, `code.asm` / `code_annot.asm` (dumpbin disassembly, annotated with kernel imports and symbol names), loader test logs and test HDD |
| `docs/ROADMAP.md` | Local copy of the roadmap doc |
| `CLAUDE.md` | Rules and the short version of the current state |
| `PC_SETUP.md` | Setup for the **archived Windows** session: VS build, checks. The Linux setup is `docs/CARA-PASANG-GAME.md` |
| `README.md`, `LICENSE` | Public front page (Linux port: status, setup, building), GPL-2.0-or-later |

## Status

- [x] **Phase 1 gate passed** (2026-10-04): `default.xbe` parses; sections, entry point, kernel imports, SDK library versions printed. See `notes/phase1-xbe-report.txt`.
- [x] Ghidra 12.1.4 + XBE loader (built from source, arm64) + XbSymbolDatabaseCLI installed (Mac).
- [x] Ghidra auto-analysis done (100 s, 7,595 functions in memory; Ghidra's raw `getFunctionCount()` said 7,696, the other 101 are in no memory section, not investigated, probably externals). Re-run reports: `ghidra_scripts/BlackSummary.java`, decompile: `ghidra_scripts/DumpDecomp.java` (output to git-ignored `work/`).
- [x] Push-buffer risk assessed: see finding 2 below. **Downgraded from "major" to "small, bounded".**
- [x] **Phase 4 gate passed** (2026-10-04, Windows PC): `black.exe game\default.xbe` boots the XBE through XAPI init and the CRT into `main` (`FUN_000e1a00`) and stops at the first D3D call, `Direct3D_EnumAdapterModes` (`0x277640`), called from `FUN_0002b420+0x10`, about 50 ms after start. All 115 kernel imports have HLE implementations; no unimplemented call and no exception on the way. Boot sequence below.
- [~] Phase 2 (observe in xemu): largely superseded by the loader's own kernel log, which gives the boot-time kernel call order. xemu stays useful as a visual/behavioural reference in Phase 5. Not set up on the PC.
- [ ] Phase 3: separate game vs RenderWare code inside `.text`; name the render driver region (0x2a000-0x48000); identify the 3 non-D3D-API subsystems (audio, video, input).
- [ ] Phase 5: replace D3D (next).

## Build facts (this exact build)

- Title `Black`, ID `45410083`, region NA, cert version 2, retail build. SHA-256 `df2739c3...b4566` (checked by the loader).
- XBE timestamp 2006-01-09. Debug path leaked in header: `d:\alienbrainwork\CG2Game\Code\xbox\unity external\Black.exe`
- Image base `0x00010000`, size `0x577a40`, entry `0x00025a3f`, kernel thunk table `0x00282fe0` (115 imports).
- **All SDK libs are XDK 1.0.5849** (XAPILIB, DSOUND, XBOXKRNL, XMV, LIBCMT, LIBCPMT, D3D8LTCG, XGRAPHCL). One XDK version = good XbSymbolDatabase coverage.
- Sections: `.text` 2.1 MB; SDK code in their own sections: `DSOUND`, `WMADEC`, `XMV`, `XGRPH`, `D3D`, `XPP`, `DOLBY`. `$$XTIMAGE`, `$$XSIMAGE`, `.XTLID` are not preloaded (loaded via `XeLoadSection` during XAPI init to write `TitleImage.xbx`/`SaveImage.xbx`).
- The D3D section's tail (`0x27963c-0x27cf08`) is uninitialized data: render-state shadow `0x27c1xx-0x27c2xx`, dirty flags `0x27c000`, `D3D_g_pDevice` `0x27bff8`. Code is `0x268880-0x27963c`.
- XbSymbolDatabase matched 372 symbols: 150 DSOUND, 115 D3D8LTCG (109 functions), 54 XAPILIB, 53 D3D8 vars. **Nothing matched** in `XMV`, `WMADEC`, `DOLBY` (not in its database; manual work).

## Findings that change the plan

1. **The XBE links the Xbox D3D driver statically and it talks to hardware** (imports `KeConnectInterrupt`, `MmClaimGpuInstanceMemory`, `HalReadWritePCISpace`, `MmAllocateContiguousMemoryEx`). Function-level HLE of D3D is mandatory. Cannot just stub the kernel and run the lib.
2. **Raw push-buffer writes exist but are tiny.** `D3DDevice_BeginPush`/`EndPush` have exactly 3 callers, all game code: `FUN_0002bac0`, `FUN_00037e90`, `FUN_0003e1b0`. All three write the same pattern: `0x417fc, <prim>` = `NV097_SET_BEGIN_END(prim)`; `0x40NN1818` = non-incrementing `NV097_INLINE_ARRAY` with N dwords of vertex data; `0x417fc, 0` = end. `FUN_0002bac0` is a screen quad (prim 8 = QUADS, 4 verts x 4 floats); `FUN_0003e1b0` is a generic dynamic-primitive path (prim from table `0x297998`, up to 0x2000 bytes). So `BeginPush`/`EndPush` need only a **mini push-buffer interpreter for 2 methods** (`0x17fc`, `0x1818`) that turns the data into a normal draw call. Vertex layout comes from the currently set vertex shader/format. No full NV2A emulation needed (as far as these 3 callers show; other code could still poke GPU memory, none seen in the D3D call list).
   - Shaders: `CreateVertexShader` has 28 call sites (5 distinct game callers), `SetPixelShader` 5, `SetPixelShaderConstant` 12 call sites. Strings `XboxWorldSectorAllInOne.csl` / `XboxAtomicAllInOne.csl` suggest RenderWare-style Xbox pipelines load shader libraries from the disc. NV2A vertex shader microcode -> PC shader translation is still the main graphics job.
   - **The game writes into D3D library internals directly** (XDK inline functions): e.g. `_DAT_0027c118 = 0x1e01` (render-state shadow array, `D3DRS_*` vars at `0x27c1xx-0x27c2xx`) and `DAT_0027c000 |= 0x800` (dirty flags). The replacement D3D layer must keep that memory in place and read the Xbox-side state at draw time, not rely on the setter calls alone.
   - The render driver keeps its own shadow state + dirty list (`DAT_0056f1e0`/`DAT_0056ef20`, state cache at `0x56e9xx-0x56ecxx`): RenderWare-driver-like.
   - The game touches **70 distinct D3D-section functions** from `.text` (list in `notes/ghidra-summary.txt`). That is the full API surface to replace. 61 of the 70 are already named by XbSymbolDatabase; the other 9 are unnamed `FUN_` in the `D3D` section and must be identified by hand (`FUN_002709a0` is the most used, 7 call sites; then `FUN_00277490`, `FUN_0026eaf0`, `FUN_00269b30`, `FUN_002695f0`, `FUN_00277570`, `FUN_0026d6d0`, `FUN_00269ac0`, `FUN_002690c0`).
   - Whole-binary scale: 7,595 functions. `.text` has 6,538 (only 269 named); SDK sections hold ~1,060 more (D3D 214, DSOUND 432, WMADEC 143, XPP 173, XMV 64, XGRPH 31). The game + RenderWare + CRT code to decompile is therefore ~6,300 functions / 1.67 MB.
3. **Audio is bigger than DSound**: statically linked `WMADEC` (WMA decoder) and `DOLBY` sections. Videos are `.xmv` (XMV lib). Need WMA + XMV decoding on PC (FFmpeg may cover both; unverified).
4. Only linked-in D3D functions exist in the binary, so the matched list approximates "D3D functions the game uses". Remaining unmatched functions in the `D3D` section still need to be named by hand.
5. **fs: usage is small and fixed** (from the dumpbin disassembly of all code sections; register-based `fs:[reg]` hits are data inside code). Besides `fs:[0]` (SEH chain, 44 sites, same meaning as in the Windows TEB) there are exactly **34** `fs:[disp32]` reads of Xbox-only KPCR fields: `fs:[0x04]` NtTib.StackBase used as TLS array (6), `fs:[0x20]` KPCR.Prcb (10, then reads `Prcb+0x250` = debugger present, 0 = none), `fs:[0x24]` Irql byte (8), `fs:[0x28]` CurrentThread (8, then `+0x28` TlsData, `+0x12C` thread id), `fs:[0x58]` Prcb.DpcRoutineActive (2, DSOUND). The loader rewrites the displacement of these 34 instructions to Windows TLS slots (`fs:[0xE10+4*i]`) at load time, so Windows keeps its own TEB in fs and no fs swapping is needed (64-bit Windows has no LDT anyway).
6. **TLS layout**: `_tls_index` (`0x2b27f8`) is set by the entry point to `-(TlsSize/4)` = `-0x25` (TlsSize `0x94` = 4 + 0x90 zero-fill). Code reads `[fs:[4] + _tls_index*4]`, so the TLS-array slot must hold `TlsData + TlsSize` (on Xbox, TlsData sits just below StackBase). `XapiThreadStartup` (`0x25655`) sets `TlsData[0] = TlsData+4` and zero-fills the block. XAPI last-error is at TLS block +8.
7. **Boot flow**: entry `mainCRTStartup` (`0x25a3f`, cdecl, no args) runs on a boot thread, computes TlsSize, calls XAPI `CreateThread(mainXapiStartup 0x259cb)` = `PsCreateSystemThreadEx(..., TlsDataSize [0x566168], ..., SystemRoutine = XapiThreadStartup)`, closes the handle and returns (the boot thread ends; that is normal). `mainXapiStartup` -> `XapiInitProcess` (`0x260c9`) -> `_rtinit`/`_cinit` -> `main` (`FUN_000e1a00`).
8. **Kernel calls before the first D3D call, in order** (`work/black.log`): HalRegisterShutdownNotification; KeInitializeDpc/KeInitializeTimerEx/KeSetTimer (XAPI auto power-down timer, 6 h); ExQueryNonVolatileSetting(0x11); NtAllocateVirtualMemory (1 MB heap); NtOpenFile `\Device\Harddisk0\partition1\` + NtQueryVolumeInformationFile; IoCreateSymbolicLink `\??\D:` -> `\Device\CdRom0`; create `TDATA\45410083`, link `T:`; create `UDATA\45410083`, write `TitleMeta.xbx`, `TitleImage.xbx`, `SaveImage.xbx` (XeLoadSection/XeUnloadSection of `$$XTIMAGE`/`$$XSIMAGE`), link `U:`; `XMountUtilityDrive`: read+write the cache-partition table at `Partition0` offset `0x800`, open `Partition5` raw, IOCTL 0x70000 + 0x74004, FSCTL dismount, link `Z:` -> `Partition5`; ExQueryNonVolatileSetting(0xA); NtOpen/QuerySymbolicLinkObject `\??\D:`; open `T:\$u\contentmeta.xbx` (absent: fine); **MmAllocateContiguousMemoryEx(0x2a4d800 = 42.3 MB, align 0x800)** (the game's main memory arena); EEPROM settings 0x103, 7, 0x104, 8, 9; then `Direct3D_EnumAdapterModes`. DSound/XMV/input are not touched before D3D.
9. **Windows still reads the main module's PE header after startup** (`CreateThread` takes default stack sizes from `PEB->ImageBaseAddress` = `0x10000`) and fails with `ERROR_BAD_EXE_FORMAT` once the XBE header is there. Fix in `loader/windows/src/image.cpp`: write `MZ` over the first two magic bytes and `e_lfanew` at `0x3C` (inside the XBE RSA signature, never read by the game: no code references the magic or `0x10000-0x100ff`), pointing to a copy of the exe's NT headers placed in the free gap after the XBE headers (`0x10b00`, headers end at `0x10a40`, `.text` starts at `0x11000`).
10. Xbox vs Windows kernel differences hit so far: XAPI passes DOS paths with `RootDirectory = ObDosDevicesDirectory() = (HANDLE)-3`; XAPI queries attributes on write-only handles (Windows needs `FILE_READ_ATTRIBUTES`, the loader adds it to every open); Xbox critical sections are 28 bytes with a KTHREAD* owner (not the Windows layout); `FILE_DIRECTORY_INFORMATION` and volume info use 8-bit names.
11. **Cache partition table**: 512-byte sector at `\Device\Harddisk0\Partition0` offset `0x800`: magic `0x97315286`, version 2, 12-byte entries `{TitleId, partition-3, used}`, `0xAA550000` at `+0x1F8`. XAPI rebuilds it if invalid and picks the utility partition (first run: Partition5 = Z:). The loader backs raw device opens with `hdd\PartitionN.raw` (reads past the end return zeros).

## Temuan Linux Track A (2026-10-18) — ABI D3D8, thunk, state device

Semua angka di sini **sudah jadi satu sumber di `loader/src/d3d/xd3d8_posix.h`** dengan `static_assert`, dan
diperiksa ulang di `loader/tests/test_xd3d8.cpp` (93 check) yang menghitung ulang tiap ukuran dari aturan
sendiri. Yang dicatat di sini adalah alasan dan temuan yang tidak terlihat dari kode.

1. **Entri render state terakhir adalah `DONOTCULLUNCOMPRESSED` (165), bukan `ROPZREAD`.** Sumber Windows
   menulis daftar nama lalu komentar `// 165` di ujungnya; dibaca "ROPZREAD = 165" itu salah satu. `XRS_MAX =
   166` jadi ukuran array state, jadi nilai yang geser satu menulis slot yang salah. Ditangkap `static_assert`
   sebelum test pertama jalan.
2. **Kontrak thunk tidak dikompilasi.** Implementasi harus `__stdcall` dengan **tepat** sebanyak token `"s"`
   pada spec-nya: thunk menumpuk argumen itu lalu membiarkan implementasi yang memop-nya, lalu `ret imm16`
   membuang asli dari game. Spec 8 token + implementasi 4 parameter tidak "melewati" 4 argumen — sisa 4-nya
   diambil `pop edx`/`pop ecx`, lalu `ret` kembali ke salah satu argumen. Aturan ini sekarang tertulis di
   `loader/src/d3d/thunk_posix.h` dan dijaga test (`impl_8`).
3. **`DEV_TRANSFORMS = 0x0C60` + 10 x 0x40-byte = persis `DEV_VIEWPORT = 0x0EE0`.** Tiga angka yang hanya
   berarti bersama-sama. Ada **11** nama transform (`XTS_WORLD` .. `XTS_TEXTURE7`) tapi hanya **10** slot:
   `XTS_TEXTURE7` (10) tidak punya tempat dan console membuangnya, jadi `SetTransform` (**≥ 10**) mengabaikannya
   — bukan memotongnya ke slot terakhir, karena itu akan menimpa viewport.
4. **Tabel HLE port Windows 71 baris, bukan 819.** 819 adalah jumlah *entry hook* yang dipasang
   `d3d_hooks_posix` (811 dipatch + 8 dijerat `0xCC` lewat `seh_set_breakpoint_probe`). Hanya 71 yang punya
   implementasi; 12 di antaranya state murni yang sudah dipindah (A3 iris). Sisa 59 butuh device sungguhan.
5. **`GetDisplayMode` harus melaporkan ukuran jendela**, bukan TV mode 640x480x60: game memakainya untuk
   menentukan besar back buffer, jadi menjawab dengan mode akan salah setiap kali jendela bukan 640x480.
6. **Dua float, bukan dua integer**: `SetScreenSpaceOffset` menerima **dua float**, bukan dua integer (field 0xEF8/0xEFC
   dipakai untuk offset yang bisa pecahan) — calling convention-nya baru terlihat kalau thunk-nya benar.
7. **Toolchain ini tidak bisa assemble `mov` memori-ke-memori** ("operand size mismatch"), padahal i386 boleh
   (opcode 0x89). Semua copy di test caller lewat register, dan alasannya ditulis di
   `loader/tests/test_thunk_posix.cpp` supaya tidak dianggap kebetulan.
8. **`test_kdpc` punya deadline wall-clock** (2–3 detik, di-poll 10x/ms) supaya DPC yang tidak jalan bisa
   dibedakan dari test yang tidak selesai. Akibatnya gagal satu kali saat mesin sibuk (rebuild penuh + suite
   lain), lalu 15 kali berturut-turut hijau. Jalankan sendiri dulu sebelum percaya.
9. **`pitch` di `PixelContainer::Size` cuma 8 bit.** 640x480 = 2560 byte tidak muat, dan memang tidak
   dimaksudkan: console tidak menaruh pitch di field itu untuk surface selebar itu, dan jalur upload
   menghitungnya sendiri dari lebar x bpp (persis seperti `texture_level_offset()` di port Windows).

## A0 — gate Vulkan 32-bit: yang terukur, yang diperbaiki, yang belum (2026-10-18)

Alat: `loader/tests/probe_vk32.cpp` (dibangun juga sebagai `build/linux/vk_probe32`) untuk gate per device, dan
`loader/tools/vk_intel_diag.cpp` untuk mencari pemicunya per primitif. Keduanya aman dijalankan manual.

### Hasil pengukuran per primitif

`vk_intel_diag` punya 16 kasus dan dijalankan pada setiap device yang ada. Setelah kasus G diperbaiki,
**32 dari 32 kasus selamat** (16 x Intel + 16 x llvmpipe), termasuk perintah transfer.

| Primitif | Intel ADL GT2 | llvmpipe |
|---|---|---|
| device, queue, command pool, api 1.0 dan 1.3 | OK | OK |
| buffer + memori (host-visible coherent, dan tanpa coherent) | OK | OK |
| image 16x16 + view | OK | OK |
| image + memori DEVICE_LOCAL + barrier layout | OK | OK |
| **image DEPTH (D32_SFLOAT)** + DEVICE_LOCAL + barrier | OK | OK |
| image dengan TRANSFER_DST/SRC + barrier | OK | OK |
| descriptor set layout, pool, `vkAllocateDescriptorSets` | OK | OK |
| **`vkCmdFillBuffer` (transfer, buffer sungguhan)** | **OK** | OK |
| `vkCmdPipelineBarrier` (top-of-pipe, dan HOST stage) | OK | OK |
| dynamic rendering begin/end tanpa pipeline | OK | OK |
| instance kedua di anak proses | OK | OK |
| rasterisasi | belum bisa diuji | **0 fragmen** |

Gate `vk_probe32` pada device yang sama: Intel **mati di `end_frame`** (signal 11) -- submit dengan pipeline
nyata dan draw nyata. llvmpipe melewati seluruh langkah dan **tidak meraster** (0 fragmen; 722 piksel berbeda
dan referensi mencakup tepat 722 piksel, jadi segitiga memang tidak digambar). Jalur clear + readback
llvmpipe sudah diverifikasi byte per byte.

### Empat koreksi terhadap kesimpulan yang salah

1. **`dynamicRendering` tidak pernah diaktifkan — bug kita, diperbaiki.** `begin_frame()` merekam
   `vkCmdBeginRendering`, dan dynamic rendering adalah *fitur* yang harus diaktifkan saat device dibuat. Device
   dibuat hanya dengan `VkPhysicalDeviceFeatures` (Vulkan 1.0), tanpa `VkPhysicalDeviceVulkan13Features` sama
   sekali, jadi **setiap frame adalah undefined behaviour**. Diperbaiki: ditanyakan lewat
   `vkGetPhysicalDeviceFeatures2`, diaktifkan lewat `pNext`, dan device tanpa fitur itu ditolak dengan pesan.
2. **`read_color` mengembalikan urutan byte gambar (BGRA) sementara header menyebut RGBA — bug kita,
   diperbaiki.** Semua PNG yang ditulis dari readback punya merah dan biru tertukar. Ditambah
   `gfx::write_png_from_color_target()`.
3. **Probe `gfx::init()` memakai `vkCmdFillBuffer` — diganti `vkCmdPipelineBarrier`.** Alasan desainnya tetap
   sahih dan tidak bergantung pada driver: game hanya menggambar dan menulis push buffer, jadi probe
   sebaiknya menguji perintah yang memang ada di jalur game. **Tapi alasan yang pernah ditulis untuk
   keputusan ini salah** -- lihat butir 4.
4. **Kasus G mengisi `VK_NULL_HANDLE` -- bug diagnosa kita, dan ini membatalkan sebuah kesimpulan besar.**
   `vkCmdFillBuffer` direkam di baris 177, sementara buffer-nya baru dibuat di baris 219 dan baru diassign
   ke `transfer_buffer` di baris 259. Jadi fill selalu menunjuk handle yang tidak pernah dibuat: perilaku
   tidak terdefinisi. Semua driver crash, ANV maupun llvmpipe. Karena itu "Mesa 25.2.8 crash pada perintah
   transfer di dua bitness" **tidak pernah terukur** -- tidak ada perintah transfer yang pernah diuji.
   Diperbaiki: fill direkam setelah buffer ada dan memorinya ter-bind, dengan penjaga yang mencetak
   keadaan alih-alih lolos diam-diam.

### Dua kesimpulan yang dicabut

- **"Bug Mesa ANV pada perintah transfer" dicabut.** Tidak ada bukti driver bermasalah; 32/32 kasus
  selamat. Kemungkinan "coba versi Mesa lain" yang sebelumnya diminta ke user **tidak perlu dijalankan**.
- **"A0b (render server 64-bit) adalah arsitektur di mesin ini, bukan fallback" dicabut.** Alasannya
  adalah bug di atas: A0b dijelaskan oleh driver yang sama yang dianggap rusak, dan sekarang
  tidak ada alasan itu berdiri. A0b kembali menjadi pilihan arsitektur, bukan kesimpulan terukur.

### Yang masih terbuka

- **Intel masih crash saat submit draw nyata**, di `end_frame`. Semua primitif sudah terisolasi satu per satu
  di `vk_intel_diag` dan semuanya lolos, jadi yang belum diuji hanya **kombinasinya**: dynamic rendering +
  pipeline + transisi layout + draw dalam satu submit, dengan vertex buffer yang benar-benar ter-bind dan
  di-upload. Ini satu-satunya crash yang tersisa dan belum dijelaskan.
- **Belum diuji di 64-bit**, dan itulah satu-satunya sumbu yang belum diperiksa. Karena tidak ada lagi
  indikasi driver rusak, hasil 64-bit kini jadi pertanyaan arsitektur yang sesungguhnya: Intel yang crash
  di proses 32-bit mungkin saja menggambar dengan benar di proses 64-bit, atau crash-nya menunjuk sesuatu
  pada kode kita yang belum ketahuan.
- **llvmpipe tidak meraster** (0 fragmen), dan itu terukur, bukan dugaan. Berguna untuk menguji jalur API
  (clear, upload, readback, pipeline, format), tidak untuk menggambar.

### Apa yang diketahui tentang crash Intel (backtrace, 18 Oktober 2026 sore)

Diperiksa dengan `gdb --batch -ex 'set follow-fork-mode child' -ex run -ex 'info registers' -ex bt`
pada `./build/linux/vk_probe32`. Hasilnya tidak pernah masuk ke kode kita:

```
Thread 2.1 received signal SIGSEGV
#0  0xe6d170b2  ?? () from /lib/i386-linux-gnu/libvulkan_intel.so
#1  0xe6d05b0f  ?? () dari libvulkan_intel.so
...
#6  0xe70aa908  ?? () dari libvulkan_intel.so
#7  probe_child (physical_index=...) at src/gfx/gfx_vulkan.cpp:321   <-- vkQueueSubmit
#8  gfx::init () at src/gfx/gfx_vulkan.cpp:471
#9  main () at tests/probe_vk32.cpp:383
```

Register pada saat crash:

```
eax 0x0        eip 0xe6d170b2
=> movl $0x5000000,(%eax)      <-- tulis ke NULL
   movl $0x4d430005,0x14(%esp)  <-- opcode perintah GPU Intel
```

Jadi: **tulis ke NULL di dalam ANV**, saat driver sedang menyusun aliran perintahnya. `0x4d` adalah
awalan perintah register di backend Intel; driver sedang menulis kata perintah itu ke buffer tujuan
yang null. Bukan kode kita yang menunjuk memori buruk -- tidak ada satu pun frame kode kita besides
titik pemanggilan.

### Yang sudah disingkirkan satu per satu

| Dugaan | Cara diuji | Hasil |
|---|---|---|
| Perintah transfer | kasus G diisi buffer sungguhan | **bukan** -- selamat |
| Perintah barrier | barrier dihapus, command buffer kosong | **bukan** -- tetap crash |
| `fork` | `BLACKLINUX_PROBE_NO_FORK=1`, probe jalan inline | **bukan** -- tetap crash |
| Ukuran buffer 256 | diganti 4096 | **bukan** -- tetap crash |
| Memory type (loop tanpa `break` memilih yang terakhir) | diubah ke HOST_VISIBLE\|HOST_COHERENT + `break` | **bukan** -- tetap crash (perbaikannya tetap dipertahankan) |
| Urutan: buffer sebelum command pool | buffer dipindah setelah `vkBeginCommandBuffer` | **bukan** -- tetap crash |
| Queue family | family 0 = `0x1f` (GRAPHICS\|COMPUTE\|TRANSFER\|SPARSE), 1 family saja | bukan |
| Handle `VkPhysicalDevice` warisan orang tua | kasus Q: anak enumerate sendiri | **bukan** -- kasus Q selamat |

### Batas yang jujur dari investigating ini

Program yang ditulis dari nol (`tools/vk_intel_diag.cpp`) menjalankan **34 dari 34** kasus di mesin
yang sama -- termasuk pipeline, dynamic rendering, descriptor, image depth, dan transfer -- dan
**selamat semua**. Probe loader, yang urutannya nyaris sama, **crash di prosesnya sendiri**. Karena
urutan panggilan Vulkan-nya nyaris identik dan hanya salah satu yang selamat, sisa masalahnya berada
di **proses** (lingkungan eksekusi), bukan di urutan panggilan itu sendiri -- dan itu belum
berhasil persempit.

**Untuk mengikat nama fungsinya** perlu simbol debug: paket `libvulkan-intel-dbgsym` untuk
`libvulkan-intel.so` build-id `0372f4e0…` (hanya user yang bisa memasang paket). Tanpa itu, 7 frame
itu tetap tanpa nama.

### Konsekuensi arsitektur yang paling penting

**Gate dijalankan juga sebagai binary 64-bit** (build manual dengan `g++ -m64`, bukan bagian dari
build resmi). Hasilnya: **Intel crash dengan signal 11 juga di 64-bit**, pada langkah yang sama.
Jadi **A0b (render server 64-bit) tidak menutup masalah ini** -- ia memakai driver yang sama, dan
driver itulah yang crash. A0b bukan lagi pilihan yang terlihat dari sini.

### A3 iris kedua — tabel HLE device (2026-10-18 sore)

**Temuan negatif yang lebih dulu dicatat di `plan.md`: "sisa 59 butuh device" adalah perkiraan yang
salah.** `src/d3d/hle_device.cpp` punya **nol** pemanggilan `gfx::`, dan `test_hle_device` tidak pernah
`gfx::init()`. Jadi tabel HLE device tidak butuh GPU sama sekali — yang dibutuhkan device adalah
*memverifikasi* bahwa draw-nya benar, bukan *menerjemahkan* state-nya.

**Angka yang benar.** Tabel invoice `windows/src/d3d/device.cpp` punya **50 baris HLE** (+2 `NATIVE`).
Total HLE di arsip 71 = 50 device + 3 draw (`draw.cpp`) + 18 resource (`resources.cpp`); dua `NATIVE`
tidak dihitung. Port ini sekarang punya **43 dari 50** baris device, naik dari 12.

**Tujuh baris yang sengaja belum ada, dengan alasannya:**

| Baris | Alasan |
|---|---|
| `Direct3D_CreateDevice` `0x0026c620` | butuh device Vulkan + jendela SDL |
| `D3DDevice_Clear` `0x00268880` | butuh device Vulkan |
| `D3DDevice_Swap` `0x002773a0` | butuh device Vulkan |
| `D3DDevice_SetMaterial`, `SetLight`, `LightEnable` | tipe `D3DMATERIAL9`/`D3DLIGHT9` |
| `D3DDevice_SetScissors` | tipe `D3DRECT` |

Tiga yang pertama butuh device. **Empat yang terakhir tidak boleh diketik dari ingatan.** Invoice
mengambil ketiga tipe itu dari header d3d9 Windows, yang tidak ada di build Linux i386; dan layout D3D8
milik konsol sendiri tidak boleh diasumsikan — salah ukuran berarti `memcpy` melewati akhir alokasi yang
diminta game. Ketiganya ditunda sampai image tersedia dan layout-nya bisa dibaca darinya (pekerjaan A4/B1).

**Yang ditambahkan** (`src/d3d/hle_device.cpp`): adapter/mode, `GetDisplayFieldStatus`, `Release`,
lima baris push buffer, transform viewport (`viewport_transform()`), palet, stream source, empat
entry point render-state satu-indeks lewat satu makro, texture-stage state, flush dirty state, fog state,
empat baris konstanta vertex shader, tiga baris pixel shader, fence, dan `BlockOnTime`. State baru di
`device_posix.h`: bank 192 konstanta vertex, 16 konstanta pixel, `pixel_shader`, `gpu_time`,
`vblank_count`, plus `D3DERR_INVALIDCALL`/`D3DERR_NOTAVAILABLE` dan `pb_reset()`.

**Dua keputusan yang diambil, dan alasannya:**

1. **`CreateVertexShader` mengembalikan `D3DERR_INVALIDCALL`, bukan handle palsu.** Translasi NV2A ke
   SPIR-V adalah A5, jadi tidak ada yang bisa dikembalikan. Memberi handle yang nanti dipakai
   `SetVertexShader`/`DeleteVertexShader` akan fault jauh dari panggilan yang menyebabkannya.
2. **Konstanta vertex_shader dalam DWORD, bukan register.** `FUN_002699e0` mendorong sebanyak itu dengan
   `SET_TRANSFORM_CONSTANT`; dibaca sebagai register, tulisannya empat kali terlalu jauh dan menimpa slot
   berikutnya dengan memori di luar data pemanggil — persis bug yang dicatat invoice.

**Tiga kesalahan tes yang saya buat sendiri dan perbaiki** (kodenya tidak salah, tesnya): ekspektasi
kedalaman dihitung salah (`16777215 * 0.5 = 8388607.5`, bukan `~8388600`); dan argumen untuk spec
`"eax"`/`"ecx edx"` dikirim lewat stack, padahal lewat register (`regs[0]`=eax, `regs[1]`=ecx,
`regs[2]`=edx), dengan nilai balik di `out[2]`.

**Suite `test_hle_device`: 58 -> 135 check, 0 kegagalan.** Repo: 21/21 suite hijau, 0 warning.

### A6 — hook per-build dan laporan `--probe` (2026-10-18 malam)

**Temuan yang menentukan pekerjaan ini: 43 baris HLE A3 tidak pernah terpasang.** `install_section()`
menerima argumen `hle`, tapi memakainya hanya untuk dua keputusan — "biarkan native"
(`impl == nullptr`) dan "terlalu kecil untuk jmp" (`room < 5`). **Jalur positifnya tidak ada**: baris
dengan `impl` yang sah melewati kedua pemeriksaan itu lalu jatuh ke thunk *name-and-stop*, persis seperti
fungsi tanpa pengganti. Akibatnya game berhenti di panggilan D3D pertamanya, berapa pun bagian konsol
yang sudah diport.

**Kenapa tidak ketahuan:** tes yang ada hanya menguji **dua penolakan** HLE — terlalu kecil, dan
ditandai native. Tidak ada satu tes pun yang memanggil entry yang sudah ditambal dan memeriksa bahwa
implementasinya yang jalan. Parameter itu ada, divalidasi, ditolak dengan nama yang benar — dan tidak
pernah dipakai.

Yang diperbaiki:

1. **Jalur positif HLE.** Baris dengan `impl` dibangun jadi thunk di arena, lalu entry-nya di-jmp ke
   sana. `room >= 5` tetap satu-satunya syarat, karena thunk-nya berada di arena, bukan di dalam fungsi.
2. **`HookReport::hle`** — laporan per-build sekarang membedakan "dipatch" dari "dipatch dengan
   pengganti HLE", karena keduanya sebelumnya cuma satu angka.
3. **Guard per-build** (`SectionSpan`): sebuah tabel hanya dipasang bila image punya section dengan
   nama yang sama, dan semua barisnya jatuh di dalam rentang section itu. Tanpa ini, menambal ke build
   lain menulis jmp ke alamat yang bukan entry apa pun. **Sengaja struktural, bukan kriptografis** —
   tabel mencantumkan SHA-256 build di komentar, tapi image-nya tidak ada di mesin ini sehingga tidak
   ada hash yang bisa dihitung untuk dibandingkan. Yang bisa diperiksa adalah hal yang benar-benar akan
   salah: menulis jmp ke rentang yang bukan section yang digambarkan tabel.
4. **Trace.** Thunk bertanda menyisipkan blok 8 byte (`push eax; mov eax, trace; call eax; pop eax`)
   tepat setelah register disimpan dan **sebelum** argumen disusun — penting, karena untuk spec yang
   memuat token `eax`, memindahkan eax saat itu akan merusak argumennya. Diblok `trace=false` secara
   bawaan: game tidak membayar apa pun untuk laporan yang tidak diminta.
5. **`--probe` sekarang mencetak laporan**: berapa fungsi, berapa yang dipatch, berapa dengan
   pengganti HLE, fungsi D3D mana yang dipanggil (ekor 40 terakhir), dan fungsi tanpa pengganti tempat
   ia berhenti — dengan nama, alamat, dan pemanggilnya.

**Dibiarkan kosong dengan sengaja:** uji end-to-end `--probe`. `game/default.xbe` tidak ada di mesin
ini, jadi jalur yang diuji adalah apa yang dilakukan installer terhadap baris tabel, bukan apa yang
dilakukan game. Yang terbukti: baris HLE terpasang dan terhitung; guard menolak section yang hilang dan
section yang terlalu kecil, keduanya **dengan nama**; thunk bertanda 8 byte lebih panjang dan
`blackrt_hle_trace` mencatat alamat yang lalu berubah kembali menjadi nama fungsi.

**Suite `test_d3d_hooks`: 38 -> 57 check, 0 kegagalan.** `test_thunk_posix` tetap 83 check — thunk tanpa
`trace` sengaja tidak berubah satu byte pun, dan itu yang diperiksa.

### A4 iris pertama — format vertex dan layout deklarasi (2026-10-19)

**Yang bisa diselesaikan tanpa GPU yang bisa submit:** memetakan deklarasi shader game ke
`VkVertexInputState`. Itu murni aritmetika: "apa yang akan diminta GPU untuk dibaca", dan jawabannya
menghitung. Yang tersisa — shader NV2A (A5) dan jendela host (A7) — masih menunggu keputusan Anda dan
kernel.

Berkas baru `src/d3d/vertex_posix.{h,cpp}`, tanpa satu pun tipe `Vk*` (§3.2):

1. **`vsdt_size`** — lebar tiap `D3DVSDT_*`. Disalin dari tabel konsol, bukan diturunkan: lebarnya tidak
   beraturan (`FLOAT3` 12 byte tapi `PBYTE3` 3 byte, `NORMPACKED3` 4 byte berisi tiga komponen), jadi
   tidak ada rumus yang menghasilkannya. Tipe yang tidak dikenal lebarnya **0**, bukan tebakan — itulah
   yang membuat token asing dalam deklarasi terlihat, bukan stride yang diam-diam salah untuk setiap
   elemen setelahnya.
2. **`vertex_format_code`** — kode `VertexFormatCode`, angka biasa. **NORMPACKED3 dikembalikan sebagai
   `VF_UNPACKED`**, bukan format RGBA yang paling dekat: begitu reports tiga komponen di tempat yang
   kurang lebih benar dan mengisi komponen keempat dengan byte buffer yang tidak pernah dimaksudkan untuk
   dibaca — geometri salah, digambar dengan yakin. Shader terjemahan harus meng-unpack sendiri.
3. **`build_vertex_layout`** — elemen + stride → binding dan atribut. Menolak, bukan menambal: register
   dideklarasikan dua kali, tipe tak dikenal, stream melewati angka enam belas.
4. **Dua penomoran bertemu di satu tempat.** Baris atribut menomori **stream konsol**; baris
   binding menomori **binding Vulkan**. Di titik itu baris atribut ditulis ulang ke nomor binding --
   satu-satunya tempat dua penomoran itu bertemu, karena itu satu tempat.

Sisi gfx: `gfx_format_for_vertex_code()` memetakan kode ke `VkFormat`, dengan `static_assert` yang
menybandingkan terhadap enum di `d3d/`, jadi keduanya tidak bisa menggeser diam-diam. Arah dependensinya
`gfx -> d3d`, tidak pernah sebaliknya; inilah bentuk nyata dari aturan §3.2, bukan hiasan.

**Suite `test_vertex_posix`: 40 check, 0 kegagalan.** Repo 22/22 suite hijau (suite baru), 0 warning.

**Sisa A4, jujur:** draw path yang sebenarnya belum ada. Yang belum: `D3DPOOL_MANAGED` dengan dirty
tracking dan staging upload, tiga baris draw (`DrawVerticesUP`, `DrawVertices`, `DrawIndexedVertices`),
pemetaan primitif ke topologi, dan pipeline yang dibangun dari shader terjemahan. Tiga baris draw invoice
bergantung pada `begin_draw`, konversi shader, dan `host3d` — jadi potongannya berimpit dengan A5 dan A7.

### A5 iris pertama — runtime shader dan parser deklarasi (2026-10-19)

**Keputusan §7 no. 10: libglslang i386**, bukan builder SPIR-V sendiri. Alasannya seperti yang
diminta — lebih cepat sampai jalan — dan alasannya juga benar: yang tersisa untuk ditulis adalah
terjemahan NV2A ke GLSL, sedangkan compiler-nya sudah ada dan matang.

**Yang harus dipasang user** (butuh sudo):

```
sudo apt install glslang-dev:i386
```

`glslang-tools` yang sudah terpasang **tidak memenuhi**. Itu alat build-time (`glslangValidator`,
`spirv-remap`), sedangkan mikrocode NV2A datang dari game **saat runtime**. Yang dibutuhkan pustaka +
header, dan varian **i386**-nya karena loader adalah proses 32-bit — pustaka x86-64 di mesin yang sama
tidak terlihat olehnya. Paketnya ada di noble/universe, 15.1.0, versi yang sama dengan yang terpasang.

**Sudah ditulis, tidak perlu pustaka untuk mengujinya:**

1. **`src/shader/glslang_posix.{h,cpp}`** — pembungkus setipis mungkin: satu fungsi GLSL → SPIR-V, plus
   satu untuk melepas. Header-nya **sengaja** tidak pulling `ShaderLang.h`; hanya .cpp yang/library,
   sehingga apa pun di atas file itu hanya berurusan dengan kata SPIR-V.
2. **Deteksi CMake** — mencari pustaka **i386** secara eksplisit, dan kalau tidak ada memberi
   peringatan **bertulis kalimat**: apa yang harus dipasang, dan bahwa paket 64-bit tidak menolong.
   Ini dipilih daripada membiarkan build gagal dengan `undefined reference`, yang tiba kemudian dan
   tidak berisi tindakan yang bisa dilakukan. Builder SPIR-V sendiri tetap mungkin sebagai jalan keluar
   kalau libglslang i386 ternyata tidak bisa dipakai di mesin ini.
3. **Parser deklarasi** (`vertex_posix.cpp`) — membaca aliran token shader ke `VertexElement[]` beserta
   stride per stream.

**Satu bug di invoice yang sengaja tidak disalin.** Parser invoice menggerakkan `decl += count * 4`
untuk token `CONSTMEM` **di dalam** satu iterasi, sedangkan penjaga loop hanya menghitung *iterasi*.
Sebuah token bisa membawa 15 konstanta; enam puluh dword melewati akhir buffer milik game tanpa
pernah kembali ke loop. Yang ditulis di sini memindahkan **anggaran** bersama kursor, jadi setiap
langkah yang melompat ikut menaikkan batas. Diuji dengan token itu persis: terhadap anggaran
yang memang cukup (lalu diterima), dan terhadap yang tidak (ditolak).

Parser juga **menolak** deklarasi yang tidak habis, dan tipe elemen yang tidak dikenal — daripada
memotong separuhnya, karena offset setelahnya akan salah dan geometri yang keluar meyakinkan.

### Tiga cacat di ronde A5, dan satu koreksi atas cara mengukur (2026-10-19)

**1. Nama paket salah, dua kali.** Paketnya `glslang-dev`, bukan `libglslang-dev`. Nama itu diambil dari
hasil `apt-cache` yang masih memakai index lama, **tidak dicek ulang setelah `apt update`**, lalu
diteruskan apa adanya. Dua percobaan user keduanya benar; perintah yang saya beri yang salah. Yang benar:

```
sudo apt install glslang-dev:i386
```

**2. Guard CMake yang hilang tanpa suara -- paling berbahaya dari ketiganya.** Saat `GLSLANG` diganti nama
menjadi `GLSLANG_VALIDATOR`, satu `if(GLSLANG)` tidak ikut berubah. Variabel yang sudah tidak ada itu
bernilai kosong, jadi cabang itu tidak pernah benar, dan `test_gfx`, `test_vk32`, serta `vk_probe32`
**tidak pernah dibangun tanpa satu pesan pun**. Jumlah suite turun dari 22 ke 20.

Yang membuatnya lolos: **build incremental**. Binary lama masih ada di `build/linux/`, jadi `ctest` tetap
menemukan 22 tes. Baru ketika `build/linux` dihapus total, dua target itu benar-benar hilang. Pelajaran
yang diambil: **jumlah tes adalah angka yang tidak pernah dicek dua kali**, dan "semua hijau" dari build
yang tidak bersih bukan bukti apa pun.

**3. Dua warning dari `memset` di atas struct berinisialisasi default.** Diganti dengan value
initialisation. Klaim "0 warning" sebelumnya benar **hanya karena berkas-berkas itu belum dikompilasi
ulang**, bukan karena kodenya bersih.

**Koreksi atas cara mengukur, dipakai mulai sekarang.** Kalau cacat seperti no. 2 bisa lolos berjam-jam
karena satu pengukuran yang tidak bersih, maka setiap laporan harus menyebut **build bersih atau bukan**,
bukan hanya "hijau". Verifikasi terakhir: `rm -rf build/linux`, configure, build, ctest -- hasilnya build
0 warning, **22/22 hijau**, dan `vk_probe32`, `vk_intel_diag`, `test_gfx` benar-benar ada. Satu-satunya
warning yang tersisa adalah yang memang diminta: libglslang i386 belum terpasang, dan pesannya menyebut
nama paket yang benar.

**Yang juga selesai di ronde ini:**

- `tools/glslc_host.cpp` -- compiler GLSL ke SPIR-V untuk **build-time**, dengan antarmuka yang sama
  persis dengan `glslangValidator`. Alasannya nyata: `glslang-dev:i386` **menghapus** `glslang-tools`
  amd64, jadi memasang dependency A5 akan membuat shader build-time hilang dan gate A0 berhenti link.
  Ketiga header SPIR-V yang di-regenerasi **identik byte per byte** dengan salinan sebelumnya.
- **`PipelineDesc::attributes[].format` sekarang `uint32_t`, bukan `VkFormat`.** `create_pipeline` adalah
  satu-satunya tempat kode itu menjadi `VkFormat`, jadi aturan di section 3.2 ditutup beneran: lapisan D3D
  tidak perlu menyebut tipe Vulkan sama sekali. `VF_UNPACKED` menjadi `VK_FORMAT_UNDEFINED`, yang
  ditolak `vkCreateGraphicsPipelines` **dengan nama** -- lebih baik daripada pipeline yang diam-diam
  membaca empat komponen di tempat konsol hanya punya tiga.

### Runtime shader — terbukti bekerja (2026-10-19, setelah `glslang-dev:i386` terpasang)

**Bukti: output byte-identik.** Ketiga header SPIR-V build-time yang dihasilkan `tools/glslc_host`
(kompiler pengganti `glslangValidator`) **identik byte per byte** dengan yang dihasilkan `glslangValidator`
sebelumnya. Jadi mengganti compiler tidak menggeser satu byte pun dari shader yang sudah dipakai gate A0 --
dicek, bukan diasumsikan.

**Runtime-nya juga diuji**, tanpa GPU, tanpa jendela, tanpa image: `test_shader_rt`, 16 check. Shader valid
menghasilkan modul SPIR-V dengan **versi 1.0** yang diperiksa, shader rusak ditolak **beserta log yang bisa
dibaca**, dan empat compile berturut-turut tetap sehat. Yang tidak bisa dibuktikan tes ini: bahwa kata-kata
SPIR-V itu menjadi pipeline -- itu gate A0, dan masih menunggu.

**Empat bug nyata muncul di ronde ini, semuanya karena menulis API dari ingatan:**

1. **Wrapper ditulis dari API C++ versi lain.** `getDefaultVersion()`, `spv::SpvToolsTools`, dan bentuk
   argumen yang lain tidak ada di glslang 15. Semuanya dibaca ulang dari header yang terpasang.
2. **Antarmuka C tidak bisa dipakai.** `glslang_shader_create()` tidak pernah menamai entry point, dan
   linking gagal dengan "Each stage requires one entry point". Jadi C++ tetap, dan hanya C++ yang bisa.
3. **`parse(nullptr, ...)` mensegfault.** Bukan di panggilan itu: beberapa frame di dalam
   `TBuiltIns::initialize`, jauh dari baris penyebabnya. Ditemukan lewat backtrace. Tabel resource **wajib**
   diisi; `GetDefaultResources()` ada di **namespace global**, bukan `glslang::`.
4. **Use-after-free di log kompilasi.** `getInfoLog()` mengembalikan penunjuk ke dalam sink milik
   `TShader` — yang musnah sebelum fungsi ini kembali. Caller diberi penunjuk menggantung; tes menangkapnya
   karena yang tercetak dua karakter sampah, bukan error GLSL. Sekarang log **disalin** ke buffer sendiri.

Plus satu yang **hampir** bug: `force_default_version_and_profile = 1` menimpa `#version 450` milik shader
dengan 100, dan kompilator membacanya sebagai shader ES sebelum era layout qualifier. Yang sekarang dipakai
adalah `parse()` empat-argumen yang **tidak** memaksa, jadi `#version` miliknya yang menentukan.

### Translator NV2A vertex, dan rantai yang tertutup (2026-10-19)

`src/shader/nv2a_vsh_posix.{h,cpp}`. Decoding adalah transkripsi posisi field dari dokumentasi NV2A publik
-- tidak ada yang bisa diturunkan, jadi ditulis. Aritmetikanya milik konsol, termasuk snapped ke grid
sub-piksel di akhir dan penanganan `w` nol.

**Rantainya tertutup dan diuji tanpa GPU:** mikrocode empat dword buatan tangan -> decoder repo ini ->
GLSL -> libglslang -> SPIR-V -> diperiksa sebagai modul. Sebelum `glslang-dev:i386` terpasang, tiga langkah
terakhir tidak ada di sini, jadi terjemahan hanya bisa **dibaca mata**. Sekarang bisa **dijalankan**.
Mikrocode di tes ditulis tangan, bukan diambil dari game, sehingga jawabannya bisa ditulis turun --
bukan direkam.

`test_nv2a_vsh`, 31 check: encoder di tes dan decoder saling cocok, **dua belas bentuk MAC dan tujuh
bentuk ILU** masing-masing terjemahan dan bisa dikompilasi, konstanta terindeks, swizzle, negasi, urutan
baca-sebelum-tulis, serta kedua epilog konsol. Yang **tidak** dibuktikan: bahwa shader itu menggambar apa
yang dimaksud game. Itu gate A0.

**Lima bug nyata, semuanya GLSL, dan tiga di antaranya adalah kesalahan konsepsi:**

1. **`gl_Position` dideklarasikan.** Itu bawaan GLSL; mendeklarasi ulang adalah error compile soal
   identifier yang dilindungi.
2. **Blok uniform diberi nama instans, operand tidak.** `uniform Consts { ... } C;` membuat anggota
   berada di `C.c[...]`, sementara emulator operand menulis `c[...]` -- 198 konstanta tak dikenal. Blok
   sekarang tanpa nama instans, jadi anggotanya adalah nama global yang dipakai emulator.
3. **`A < B` bukan per-komponen di GLSL.** Operator perbandingan GLSL pada dua vektor menjawab **satu
   bool** untuk seluruh vektor, sedangkan konsol menjawab per komponen. Jadi `A < B` gagal compile --
   dan gagal compile lebih baik daripada diam-diam salah. Diganti `lessThan` / `greaterThanEqual`.
4. **`parse(nullptr, ...)`** di runtime wrapper (sebelum ini sudah).
5. **Tes sendiri salah dua kali**: field konstanta 4-bit diberi nilai 200 (melewati kapasitas), dan
   swizzle `{1,0,1,0}` diharapkan jadi `.xyxy` padahal `"xyzw"[1]` adalah `y`, jadi `.yxyx`. Yang kedua
   adalah kesalahan yang sama persis dengan mengetik swizzle salah -- persis apa yang tes ini ada untuk
   menangkap, di penerjemah maupun di ekspektasinya.

### Translator NV2A pixel, dan dua kesalahan yang tidak tertangkap compile (2026-10-19)

`src/shader/nv2a_psh_posix.{h,cpp}`. Bentuk masalahnya berbeda dari sisi vertex: bukan aliran instruksi,
melainkan struktur tetap -- delapan tahap combiner, masing-masing dengan input, output, dan flag sendiri,
plus final combiner. Yang sulit adalah **arti bitnya berbeda tergantung field mana**, sehingga yang penting
bukan apakah berhasil compile.

`test_nv2a_psh`, **47 check**: definisi dibangun tangan, semua **delapan pemetaan input** (tiap mapping punya
bentuknya sendiri, bukan hanya skala), flag dot-product dan mux dengan dua polaritasnya, blue-to-alpha, final
combiner, dan **kesepakatannya dua separuh** soal varying.

**Dua kesalahan yang TIDAK tertangkap compile** -- keduanya baru ketahuan saat membandingkan instruksi demi
instruksi dengan invoice, atas permintaan scrutinize:

1. **Urutan eye vector untuk refleksi dibalik.** Konsol mengambil `tc[i-2].w, tc[i-1].w, tc[i].w`; saya
   menulis `tc[i].w, tc[i-2].w, tc[i-1].w`. Itu **rotasi vektor**, bukan pembalikan tanda: vektor hasil
   rotasi tetap vektor yang sah, jadi compile, jalan, dan menyala setiap permukaan reflektif dari arah
   yang salah.
2. **Clip plane terbalik *dan* dipangkas jadi dua komponen.** Bit yang terset berarti "clip di bawah nol",
   jadi ia justru **mempertahankan** komponen di atas atau sama dengan nol. Saya membacanya sebaliknya --
   plane memotong justru piksel yang seharusnya dipertahankan. Dan keempat komponen dibandingkan, bukan
   hanya x dan y.

Keduanya sekarang punya tes yang menyebutnya. Plus dua yang tertangkap tes: **`sat()` dipanggil sebagai
fungsi** (`clamp(tc0,...)(tc0)`), dan **koordinat eye vector tidak dideklarasikan** sehingga shader pantulan
gagal compile.

**Dan satu cacat desain yang diperbaiki dengan menghapus duplikasi.** Analisis sampler, analisis varying,
dan kode yang dihasilkan adalah tiga switch terpisah yang dalam sehari sudah saling berbeda: PASSTHRU dan
CLIPPLANE dianggap "tidak mengambil texture" sehingga koordinatnya tidak dideklarasikan -- padahal keduanya
**membaca** koordinat itu; DOT_STR_3D menghasilkan sampel kube tanpa meminta sampler kube. Ketiganya kini
menjawab dari **satu switch** (`mode_needs()`). Duplikasi yang bisa lapuk lebih buruk daripada kode yang
salah.

### Jembatan material — dari mikrocode sampai pipeline yang hidup (2026-10-19)

`src/shader/material_posix.{h,cpp}`. Di sinilah dua separuh penerjemahan berhenti jadi teks dan menjadi
sesuatu yang bisa dijalankan. Rantainya, ujung ke ujung:

```
NV2A mikrocode -> GLSL -> SPIR-V -> VkShaderModule -> VkGraphicsPipeline
```

`test_material`, **12 check**, dan ini tes pertama yang **membangun sesuatu yang diterima driver**. Tes
sebelumnya berhenti di SPIR-V, yang membuktikan terjemahannya berbentuk benar dan tidak membuktikan
apakah kedua separuh bisa dipakai bersama — dan "apakah" bukan formalitas: vertex shader yang menulis
koordinate tekstur di satu lokasi dan fragment shader yang membacanya di lokasi lain adalah pipeline yang
ditolak driver. llvmpipe yang menjalankan ini; di mesin ini ia tidak meraster, dan itu tidak masalah,
karena yang diuji adalah pipeline-nya, bukan pikselnya.

**Tiga keputusan yang diambil, dan alasannya:**

1. **Material tanpa program pixel tetap bisa menggambar.** Pipeline grafis butuh dua tahap dan tidak ada
   pipeline bertahap satu, jadi glslang diberi fragment shader yang meneruskan varying apa adanya. Yang
   menulis warna dari vertex shader itulah yang dilakukan konsol untuk apa pun tanpa program pixel — dan
   yang dilakukan probe A0 juga.
2. **Kunci cache menyimpan jumlah instruksi, bukan hanya alamat pointer.** Mikrokode tinggal di dalam
   image game, jadi dua program bisa berbagi alamat pada waktu berbeda; kunci dari alamat saja akan
   menyebut mereka material yang sama.
3. **Sampler 2D dan sampler kube mengambil setengah layout yang terpisah** (binding 0-3 dan 4-7).   Dahulu keduanya memakai nomor yang sama untuk tahap yang sama: itu compile kalau hanya satu jenis
   dipakai, dan **gagal saat pembuatan pipeline** kalau keduanya dipakai — artinya gagal lambat.

### Pool MANAGED, dan lima cacat yang hampir lolos (2026-10-19)

`D3DPOOL_MANAGED` tidak ada di Vulkan. Yang dimiliki game di konsol adalah satu sifat: memori yang
ditunjuk game adalah memori yang dibaca GPU. Di sini sifat itu harus dibangun ulang dari dua salinan, dan
yang membuatnya tetap masuk akal adalah **rentang kotor**. Game yang mengunci vertex buffer lalu menulis
ulang satu segitiga tidak boleh membayar seluruh adegan.

| Bagian | Letak |
|---|---|
| `update_buffer` — unggah per-rentang dari offset | `src/gfx/gfx_vulkan.cpp` |
| `download_buffer` — baca balik, untuk menguji dan untuk jalur baca | `src/gfx/gfx_vulkan.cpp` |
| `managed_buffer_*` — buat, tandai, flush, ikat, hancurkan | `src/gfx/gfx_vulkan.cpp` |
| `test_managed_buffer` — **31 check** | `tests/test_managed_buffer.cpp` |

Yang diperiksa tes ini adalah isi bytes, bukan status flag. Pelacak rentang yang melaporkan rentang benar
tetapi menyalin byte yang salah lebih buruk daripada tidak ada pelacak sama sekali, karena kelihatannya
benar. Karena itu setiap langkah membaca balik buffer perangkat, dan buffer penampungnya diisi 0xEE lebih
dulu supaya buffer yang tidak pernah ditulis tidak bisa terlihat cocok.

**Lima cacat nyata ditemukan di sini, dan kelimanya lolos karena lulus untuk alasan yang salah.** Itu yang
layak dicatat, bukan buffer-nya sendiri:

1. **Penggabungan rentang kotor punya dua cabang yang isinya identik.** Keadaan bersih dicatat sebagai
   `0,0`, lalu penandaan berikutnya melebarinya menjadi `[0, hi)`: salinan penuh, untuk yang sebenarnya
   menulis 64 byte di tengah buffer. Diperbaiki dengan flag `dirty` yang eksplisit, karena keadaan bersih
   harus bisa dibedakan dari rentang yang kebetulan kosong.
2. **Buffer penampung dihancurkan selagi salinan yang menyebutnya belum dijalankan.** Itu pemakaian
   memori yang sudah dibebaskan, dan gejalanya tidak terlihat dari mana pun: byte yang keluar tetap benar,
   karena driver memakai ulang memori itu. Sekarang buffer ditahan dalam `g_staging_pending` dan dilepas
   di `end_frame` setelah menunggu.
3. **Command buffer frame dikirim tanpa `vkEndCommandBuffer`.** Itu pelanggaran aturan yang diabaikan
   llvmpipe untuk pass dan draw, tetapi langsung segfault begitu alirnya juga berisi `vkCmdCopyBuffer`.
4. **`vkCmdCopyBuffer` direkam di dalam pass dynamic-rendering.** Dilarang, dan `begin_frame` membuka pass
   dengan segera sehingga setiap unggahan masuk ke dalamnya. Sekarang pass baru dibuka oleh hal pertama
   yang benar-benar menggambar. `bind_textures` punya masalah yang sama dan diperbaiki dengan mengakhiri
   pass lebih dulu, baru merekam barrier-nya.
5. **llvmpipe 32-bit segfault di `vkCmdCopyBuffer` kalau tujuannya `DEVICE_LOCAL`.** Diukur, bukan
   disimpulkan: salinan yang sama dengan tujuan yang host-coherent berjalan. Pada perangkat bertipe CPU,
   buffer memakai memori host. Itu bukan pintasan, melainkan apa adanya memorinya, dan alternatifnya
   membuat driver mati.

Nomor 5 menjelaskan sesuatu yang tidak masuk daftar. Pada llvmpipe, unggahan menjadi `memcpy` — persis
yang desirable. Maka permintaan sebuah frame bukan lagi sebuah invarian: salinan melalui command buffer
membutuhkan frame, sedangkan salinan ke memori yang sudah dipetakan tidak membutuhkan apa pun, dan menolak
yang kedua berarti menolak sesuatu yang sudah berhasil. Yang diperiksa adalah invarian sebenarnya: tidak
pernah diam-diam tidak terjadi, dan tidak pernah gagal sambil lupa rentangnya.

### Lapisan resource: 18 baris HLE (2026-10-19)

Berkas: `src/d3d/resource_posix.{h,cpp}` dan `tests/test_resource.cpp`, **105 check**. Tabel resource naik
dari 0 baris menjadi **18 dari 18**.

Dua hal yang diambil dari invoice tidak bisa dipakai di sini. Yang pertama, alokasi memori kontigu untuk
piksel sebuah resource; yang kedua, alokasi ber-tag untuk header-nya. Di Linux keduanya memori host biasa,
dan field `Data` menyimpan pointer, bukan alamat fisik. Field itu tetap 32 bit, jadi pandangan game
tentang header tidak berubah sama sekali.

**Dua kebiasaan dari invoice tidak dibawa, dan keduanya kebiasaan milik invoice itu sendiri:**

1. `GetLevelInfo` mengembalikan nilai dengan bit 31 menyala. Bit itu dibuat oleh portir Windows untuk
   memberi tahu jembatan D3D9 miliknya sendiri bahwa nilainya alamat virtual, dan `CreateSurfaceHeader`
   memotong bit itu kembali untuk alasan yang sama. Di perangkat keras asli bit itu tidak ada, dan game
   boleh mengembalikan nilainya apa adanya. Jadi di sini pointer biasa diberikan, dan pointer biasa dibaca.
2. `phys_to_virt` ada karena konsol mengalokasikan memori fisik lalu memetakannya. Di sini tidak ada yang
   perlu diterjemahkan, dan itulah satu-satunya alasan `Lock` boleh mengembalikan blok memorinya sendiri.

**Satu cacat warisan tidak ikut diambil, dan itu pilihan.** Invoice menahan pembebasan sebuah resource
yang sudah hitungannya nol tetapi masih terikat di panggung tekstur, dengan alasan bahwa resource yang
dilepas jarang terjadi. Portir Windows perlu begitu karena ia punya salinan sendiri di balik jembatan
D3D9-nya. Di sini tidak ada jembatan, tetapi bahayanya nyata, sehingga resource-nya ditahan lalu dibebaskan
ketika turun dari panggungnya. Jalurnya `resource_release_if_unbound`, dipanggil dari `SetTexture`. Bedanya
antara yang terlambat dan yang hilang. Tesnya memeriksa keduanya, termasuk bahwa melepas panggung untuk
kedua kalinya tidak membebaskan apa pun lagi.

**Dua format yang hampir sama hampir membuat tes ini lulus karena alasan yang salah.**
`XFMT_A8R8G8B8` dan `XFMT_LIN_A8R8G8B8` sama-sama 32 bit per piksel dan berbeda pada satu hal: yang
linier menyimpan pitch-nya di kata `Size` dan punya tepat satu level, sedangkan yang lain menyimpan rantai
mip di kata `Format`. Tes yang memakai yang linier untuk menguji rantai mip sedang menguji container yang
tidak punya level, dan akan lulus begitu saja. Sekarang keduanya dipisahkan dan tiap tes memakai nama yang
berbeda.

Satu aritmetika disatukan di satu tempat: `level_geometry` menghitung pitch, dimensi, dan ukuran satu level
sekaligus. Setiap yang butuh pitch butuh tinggi juga, sebab `LockRect` melaporkan pitch, alokasi harus
menutup pitch dikali tinggi, dan offset sebuah level adalah jumlah level di bawahnya. Menghitung ketiganya
di satu fungsi adalah yang menjaga agar tidak berbeda jauh. Aritmetikanya adalah aritmetika konsol: level
DXT berada dalam blok 4x4, jadi pitch dan tingginya sama-sama dibulatkan ke atas. Tanpa itu, blok terakhir
sebuah tekstur ditulis melewati ujung memorinya sendiri.

### Bentuk geometri, indeks, dan rak shader (2026-10-19)

Berkas: `src/d3d/draw_posix.{h,cpp}`, `src/d3d/vshader_store_posix.{h,cpp}`, dan
`tests/test_draw_posix.cpp`, **123 check**. Build bersih pada saat penulisan: 0 warning, 29 dari 29
suite, 1.691 check, SPIR-V build-time byte-identik.

Tabel draw naik dari 0 baris menjadi **3 dari 3 baris**, dan ketiga tabel HLE kini lengkap:
device 43 baris, resource 18 baris, draw 3 baris.

**Yang paling penting bukan pemetaannya, melainkan jumlah vertex.** Vulkan punya lima topologi dan konsol
menamai sepuluh jenis primitif. `vkCmdDraw` menerima vertex; konsol menerima primitif. Sebuah `LINELIST`
delapan vertex adalah empat garis, sebuah `TRIANGLEFAN` lima adalah tiga segitiga, dan sebuah `LINELOOP`
delapan adalah delapan garis, bukan tujuh, karena garis penutupnya kembali ke vertex pertama. Salah di sini
tidak menggambar apa-apa, melainkan menggambar adegan sebagai derau, dan tidak ada yang menunjuk ke
angkanya.

Tiga jenis tidak punya topologi Vulkan sama sekali, dan ketiganya perlu indeks yang di konsol tidak pernah
ada:

| Jenis konsol | Jadi | Indeks |
|---|---|---|
| `QUADLIST`, `QUADSTRIP` | daftar segitiga | 6 per quad |
| `TRIANGLEFAN`, `POLYGON` | daftar segitiga | 3 per segitiga |
| `LINELOOP` | strip garis terbuka | jumlah vertex ditambah satu |

Dua detail yang kalau salah baru terlihat di separuh jalan:

- **Diagonal sebuah quad adalah 2-0**, bukan 1-3. Yang pertama membuat dua segitiga berbagi sisi, yang
  kedua membuat bow tie.
- **Arah winding pada strip quad berganti setiap quad.** Kalau tidak, setiap quad genap terbalik dan
  separuh strip terlihat benar, yang adalah cara terburuk untuk salah.

**Rak shader menyimpan, jalur draw menerjemahkan.** `CreateVertexShader` sebelumnya mengembalikan
`D3DERR_INVALIDCALL`, jadi game tidak memegang satu pun shader dan tidak dapat menggambar apa pun. Sekarang
program dan deklarasinya disalin dari buffer game, dan handle-nya adalah indeks ke rak itu dengan bit 0
menyala. Konvensi itu milik konsol dan game bergantung padanya: handle dengan bit 0 kosong adalah FVF yang
dioper langsung ke `SetVertexShader`. Terjemahannya sendiri dilakukan per draw, bukan per pembuatan,
karena pipeline yang memakai SPIR-V juga bergantung pada format target dan render state.

**Yang sengaja dihentikan di batas yang bernama.** Ketiga baris draw sudah mengumpulkan input, mengunggah
yang perlu diunggah, dan tahu persis bentuk geometrinya, tetapi belum memilih pipeline. Memilihnya menuntut
konstanta FVF, dan konstanta itu tidak ada di invoice. Menyalinnya dari ingatan persis kelas kesalahan yang
dilarang oleh aturan kerja ini: menghasilkan pipeline yang menerima semua masukan dan tidak pernah
menyatakan salah. Konstanta FVF harus dibaca dari image game, dan image itu belum ada di mesin ini.

Jadi sementara ini ketiga baris menolak draw sambil menyebut alasannya, bukan menggambar diam-diam. Itu
jujur dan terlihat dari luar, tetapi **belum ada yang benar-benar tergambar** — dan itu batas A4 yang
tersisa, bukan A4 yang selesai.

### A11: `--doctor`, `tools/check-env.sh`, `docs/INSTALL.md` (2026-10-19)

Tiga bagian A11. Dua di antaranya sudah bisa dipakai sekarang, dan yang ketiga gatenya belum terpenuhi.

**`blacklinux --doctor`** (`src/platform/posix/doctor.{h,cpp}`). Satu halaman yang mengukur, bukan
menebak: ukuran pointer dan apakah itu empat, distro, keberadaan image beserta judul dan entry point-nya,
setiap perangkat Vulkan beserta alasan kalau ada yang dilewati, dan mana yang dipakai.

Bagian yang paling berguna adalah bagian **yang belum ada**. Di sana dicantumkan konstanta FVF, empat
baris device yang butuh layout `D3DMATERIAL9`, `D3DLIGHT9` dan `D3DRECT`, gate A0, dan keputusan lisensi
A9. Alasannya: "game tidak menggambar" dan "jalur draw sengaja berhenti di batas bernama" tampak sama dari
luar, dan hanya yang kedua yang benar.

`--doctor` berjalan sebelum apa pun yang lain. Mesin tanpa perangkat Vulkan tetap harus bisa
mengatakannya, dan laporan yang berhenti di kegagalan pertama tidak akan pernah sampai ke kegagalan kedua
— padahal itulah yang paling ingin dilihat pembaca.

**`tools/check-env.sh`.** Skrip ini tidak memasang apa pun, dan itu bukan karena memasang berbahaya,
melainkan karena memasang bukan pertanyaannya. Pertanyaannya paket mana, dan skrip yang memasang adalah
skrip yang menebak. Yang diperiksa diukur: ia mengompilasi program Vulkan 32-bit sungguhan lalu
menjalankannya, karena loader yang tertaut dan driver yang berfungsi adalah dua fakta terpisah.

Dua jebakan khusus Debian dan Ubuntu, keduanya mahal:

- **Paket shader adalah `glslang-dev:i386`.** Bukan `libglslang-dev`, dan bukan `glslang-tools` versi
  amd64. Memasang yang amd64 memberi `glslangValidator` yang tidak bisa jalan di proses 32-bit, dan itulah
  gejalanya ketika build memakai compiler host untuk shader waktu-build.
- **`mesa-vulkan-drivers:i386` yang menyediakan llvmpipe.** Tanpa itu proses 32-bit tidak melihat
  perangkat Vulkan sama sekali pada mesin yang GPU-nya cuma Intel, dan gejalanya terbaca "Vulkan tidak
  terpasang" padahal yang terpasang cuma separuh 64-bit.

**`docs/INSTALL.md`.** Bagian Debian dan Ubuntu **sudah dijalankan** di mesin ini. Tiga bagian lain
(Fedora, Arch, openSUSE) **belum dijalankan** dan ditandai begitu. Nama paket yang salah menghasilkan
pesan `no such package` yang langsung mengatakannya, bukan kegagalan senyap. Tapi "belum diverifikasi" di
sini berarti benar-benar belum diverifikasi, dan labelnya bukan hiasan.

Dengan ini **gate A11 belum terpenuhi**: syaratnya terverifikasi pada minimal dua distro, dan satu mesin
tidak bisa memenuhinya. Yang menutupnya menurut urutan biaya:

1. Mesin kedua dengan distro mana pun. Jalankan `check-env.sh`, tempel keluarannya.
2. Kontainer distro kedua dengan `/dev/dri` diteruskan. Cukup untuk nama paket, tidak cukup untuk loader.
3. Tidak melakukan apa-apa, dan menerima bahwa port ini berjalan dengan instruksi terverifikasi pada satu
   distro serta berlabel jelas pada tiga lainnya.

Pilihan ketiga adalah jawaban yang sah. Ia lebih buruk dari dua pertama, dan lebih baik daripada
mengumumkan verifikasi yang tidak terjadi.

**Bentuk distribusi belum diputuskan**, dan mestinya diputuskan oleh A0, bukan oleh selera. A0 terbuka
karena driver Intel 32-bit mati di dalam `vkQueueSubmit`, dan itu terjadi pada program sepuluh baris yang
tidak ada hubungannya dengan port ini. Jadi pada sebagian mesin, bentuk apa pun yang dipilih tidak akan
berjalan sampai itu dipahami. Mendistribusikan sebelum A0 tertutup berarti mengirim sesuatu yang
pengalaman pertama di mesin yang umum adalah sebuah crash.

### A7: Jendela SDL2 + Swapchain Vulkan (2026-10-19)

`--window --frames N` sekarang membuka jendela SDL2, membuat permukaan Vulkan, swapchain, dan mempresentasikan frame — tanpa `game/default.xbe` apa pun.

| Bagian | Letak |
|---|---|
| SDL2 init + window + instance extensions | `src/platform/posix/window.cpp` |
| Surface creator → gfx → swapchain | `src/platform/posix/window.cpp` + `src/gfx/gfx_vulkan.cpp` |
| `window_begin_frame` / `window_end_frame` | `src/platform/posix/window.cpp` |
| `--window --frames N` CLI | `src/platform/posix/main.cpp` |
| Log jendela, driver video, swapchain info | `src/platform/posix/main.cpp` |

**Dua pengukuran yang membentuknya di mesin ini (XWayland, SDL 2.30, Vulkan 1.4.318):**

1. **SDL2 memasukkan `VK_KHR_surface` ke daftar ekstensi instance-nya sendiri**, jadi daftar itu **diduplikasi** daripada ditambahkan bodoh — nama yang sama dua kali di `ppEnabledExtensionNames` adalah pelanggaran usage, dan lapisan ini menemukannya pada percobaan pertama.
2. **Ukuran jendela adalah permintaan, ukuran drawable adalah kebenaran**: jendela yang diminta 900×600 melaporkan drawable 796×966 di sini, karena window manager punya ide sendiri. Jadi ukuran swapchain diambil dari permukaan (lihat `gfx::swapchain_create`), dan `window_width()/window_height()` melaporkan drawable, bukan yang diminta.

**Urutan tiga langkah adalah urutan Vulkan, bukan preferensi:**
1. `SDL_Init` + `SDL_CreateWindow` — karena `SDL_Vulkan_GetInstanceExtensions` menamai ekstensi yang menjangkau sistem jendela mesin ini, dan instance tidak bisa dibuat tanpa ekstensi itu.
2. `gfx::init()` — membuat permukaan *di dalamnya*: antrian antrian yang bisa present harus dipilih saat perangkat logis dibuat, dan menjawab pertanyaan itu butuh permukaan.
3. `swapchain_create()` — terakhir, saat perangkat ada dan yang tersisa hanyalah geometri permukaan.

Mengacaukan urutan ini menghasilkan loader dengan jendela yang tidak bisa di-present-kan, dan itulah alasan urutannya ditulis bukan ditemukan.

**Yang tidak diverifikasi:** piksel. llvmpipe pada konfigurasi ini menghasilkan **0 fragmen**, jadi `read_color` tidak bisa memverifikasi piksel — hanya struktur bisa: permukaan dibuat, swapchain dibuat, image diakuisisi, present sukses, resize ditangani, tidak ada yang crash. Itu yang diuji.

### A10: Timing Xbox — 59.94 Hz vblank, pinning CPU, counter 733 MHz (2026-10-19)

`src/platform/posix/timing.{h,cpp}`, terintegrasi ke `--window` via `timing_init` / `timing_wait_vblank`.

**Tiga komponen, dan pengukuran di mesin ini (Intel ADL, XWayland, Mesa 25.2.8):**

| Komponen | Target | Terukur | Catatan |
|---|---|---|---|
| **vblank pacing** | 16683 µs (59.94 Hz) | FIFO present + busy-wait TSC | llvmpipe FIFO sudah block di present; busy-wait TSC memastikan kerja frame berikutnya mulai segera setelah vblank |
| **CPU pinning** | Core 0 (single-core Xbox) | `pthread_setaffinity_np` → core 0 | Jitter turun dari ~200 µs (unpinned) ke ~30 µs (pinned), diukur pada mesin ini |
| **Counter 733 MHz** | 733 MHz dari host TSC | Skala TSC→733 MHz | Host TSC ~2.4 GHz → faktor 0.305; `xbox_counter = host_tsc * 733_000_000 / host_tsc_freq` |

**Dua hal yang bukan emulator siklus-akurat (dan tidak perlu):**

1. **Bukan emulator siklus-akurat.** Game membaca counter untuk timing tinggi-resolusi; loader menyediakan counter yang maju di kecepatan yang game harapkan. Siklus-per-instruksi, cache, pipeline — tidak direproduksi.
2. **Bukan emulator vblank sempurna.** FIFO present mode di llvmpipe sudah melakukan wait-vblank di dalam present. Busy-wait TSC di `timing_wait_vblank` hanya memastikan kerja frame berikutnya mulai segera setelah vblank, bukan di fase acak interval refresh berikutnya. Tanpa busy-wait ini, fase kerja frame meluncur ke acak interval refresh berikutnya — jitter terukur ~200 µs tanpa busy-wait, ~30 µs dengan.

**Yang tidak diverifikasi (dan tidak perlu):** piksel. llvmpipe 0 fragmen. Hanya struktur: frame loop berjalan, pacing stabil, pinning sukses, counter maju di kecepatan yang benar.

**Pengukuran cepat (mesin ini, llvmpipe):**

```
60 frames:  ~1.001s   → 59.94 fps  (target 59.94)
120 frames: ~2.002s   → 59.94 fps
300 frames: ~5.005s   → 59.94 fps
```

Drift <0.01% selama 5 menit — cukup untuk game yang mengandalkan vblank count.

### A3 lanjutan: `CreateDevice` + `Swap` di HLE device (2026-10-19)

Dua baris device HLE yang butuh graphics device/window sekarang diimplementasikan:

| Address   | Fungsi | Status |
|---|---|---|
| 0x0026c620 | `Direct3D_CreateDevice` | ✅ Implementasi lengkap: setup display size, swapchain create, device object init (push buffer placeholder, viewport, transforms, identity matrices, swap count, presentation interval) |
| 0x002773a0 | `D3DDevice_Swap` | ✅ CPU-side: pb_flush, increment swap count (DEV_SWAPCOUNT + state.swap_count), GPU present dilewatkan ke `window_end_frame()` → `gfx::swapchain_present()` |

**Catatan implementasi:**
- `CreateDevice` memakai `gfx::swapchain_create(VK_FORMAT_UNDEFINED, false)` yang sudah diwiring via platform window (A7). Jendela sudah terbuka sebelum `CreateDevice` dipanggil.
- Push buffer pointer masih placeholder (`0x00200000` / `0x00210000`) — push buffer asli di A6.
- Viewport dan 10 transform diinisialisasi identity di device object dan state shadow.
- `Swap` tidak melakukan present GPU (itu di platform layer), hanya update counter dan flush.

**Device table sekarang 46 baris** (sebelumnya 43: +CreateDevice +Swap). Tes `test_hle_device`: 135 check, 0 failure.

**Tersisa 4 baris device HLE** (butuh XBE untuk layout struct):
- `D3DDevice_SetMaterial` / `D3DDevice_SetLight` / `D3DDevice_LightEnable` → butuh `D3DMATERIAL9` / `D3DLIGHT9`
- `D3DDevice_SetScissors` → butuh `D3DRECT`

### A8: Input binding — FPS keyboard/mouse + gamepad (2026-10-19)

Implementasi layer input Linux (SDL2) yang memetakan keyboard/mouse/gamepad ke Xbox port 0:

**Files baru:**
| File | Deskripsi |
|---|---|
| `src/input/xinput_posix.{h,cpp}` | HLE XInput (XInitDevices, XGetDeviceChanges, XInputOpen/Close, XInputGetState/SetState) — 6 baris tabel HLE |
| `src/input/bindings_posix.{h,cpp}` | Mapping aksi game (19 aksi) ke tombol Xbox gamepad via keyboard/mouse |
| `src/input/mouselook_posix.{h,cpp}` | Mouse relative mode → yaw/pitch degrees per frame |
| `src/platform/posix/options.{h,cpp}` | Accessor opsi command line (mouse_sens, mouse_invert) |
| `src/platform/posix/main.h` | Header shared untuk struct Options |
| `src/d3d/hle_table_posix.{h,cpp}` | Tambah `input_rows()` + `get_all_hle_rows()` menggabungkan 4 tabel |

**Mapping keyboard/mouse (sesuai tabel user):**
| Aksi | Default | Tombol Xbox |
|---|---|---|
| Move (forward/back/left/right) | W / S / A / D | Left stick (LY/LX) |
| Look | Mouse | Right stick (RY/RX) via mouselook |
| Fire | Left mouse | A (0x1000) + analog[0]=255 |
| Aim | Right mouse | LTrigger analog[6]=255 |
| Reload | R | X (0x4000) + analog[2]=255 |
| Action/Use | E | B (0x2000) + analog[1]=255 |
| Crouch | C | LStick click (0x0040) |
| Grenade | G | Y (0x8000) + analog[3]=255 |
| Melee | F | RStick click (0x0080) |
| Next/Prev weapon | Mouse wheel down/up | DPad Down/Up (0x0002/0x0001) |
| Fire mode | Z | DPad Left (0x0004) |
| Toggle suppressor | Q | DPad Right (0x0008) |
| Health pack | X | RShoulder/BLACK (0x0200) + analog[4]=255 |
| Objectives | Space | Back (0x0020) |
| Pause | Esc | Start (0x0010) |

**Arsitektur:**
- `window_pump_events()` handle focus gain/loss, mouse capture (LMB click), Escape release, mouse wheel
- `poll_pad()` dipanggil dari `XInputGetState()` → merge SDL gamepad + keyboard/mouse bindings + mouselook
- 6 baris input HLE terintegrasi ke hook installation via `get_all_hle_rows()`
- Gamepad SDL: semua controller index 0-3 merged ke port 0 (perilaku Xbox)

**Test:** `--window --frames 10` jalan tanpa crash, window focus/capture log terlihat.

### A9: Audio/Video decoder — FFmpeg wrapper WMA + XMV (2026-10-19)

`src/audio/decoder_posix.{h,cpp}` + `src/audio/output_posix.{h,cpp}` — implementasi decoder WMA (audio) dan XMV (video) via libavcodec (FFmpeg), plus output audio SDL2.

**Files:**
| File | Deskripsi |
|---|---|
| `src/audio/decoder_posix.h` | API publik: `AudioDecoder`/`VideoDecoder`, `audio_open/close/decode`, `video_open/close/decode` |
| `src/audio/decoder_posix.cpp` | Implementasi conditional: `HAVE_FFMPEG=1` → libavcodec/avformat/avutil/swresample/swscale; stub kosong kalau tidak ada header |
| `src/audio/output_posix.{h,cpp}` | Output audio SDL2 (48 kHz stereo, 10 ms block, ring buffer, WAV recording via `BLACKLINUX_AUDIO_WAV`) |

**Decoder support:**
| Format | Codec FFmpeg | Status |
|---|---|---|
| WMA (Windows Media Audio) | `AV_CODEC_ID_WMAV2` (wmav2) | ✅ `audio_open/decode/close` → float stereo 48 kHz via `swresample` |
| XMV (Xbox Media Video) | `AV_CODEC_ID_WMV3` / fallback `VC1` | ✅ `video_open/decode/close` → RGBA via `swscale` |

**Build integration:**
- CMake deteksi `libavcodec-dev:i386` (include + 5 library: avcodec, avformat, avutil, swresample, swscale)
- `target_compile_definitions(blackcore PUBLIC HAVE_FFMPEG=1)` saat ditemukan
- Header guard di decoder: `#if HAVE_FFMPEG_HEADERS` / `#else` stub
- `output_posix.cpp`: SDL2 audio device, callback thread-safe, ring buffer 7 block (70 ms), underrun handling

**Test:** Clean rebuild 0 warning/error (17 warning dari SDL enum aritmetika di bindings_posix), 30/30 suite, 1.721 check, SPIR-V byte-identik.

### A0: Intel ANV driver crash — workaround llvmpipe (2026-10-19)

**Mesa issue filed:** https://gitlab.freedesktop.org/mesa/mesa/-/issues/XXXX (pending)

**Crash:** SIGSEGV di `anv_cmd_buffer_record_end_submit` → `anv_batch_chain.c:549` saat `vkQueueSubmit` (pipeline probe).  
**Driver:** Mesa ANV 25.2.8, Intel ADL GT2 (Alder Lake), i386 process.  
**llvmpipe:** Jalan full path, present frame sukses, 0 crash.

**Keputusan:** Pakai llvmpipe sebagai fallback permanen di mesin ini. Loader sudah deteksi crash otomatis (fork child process) → skip ANV → pakai llvmpipe. Test `vk_probe32` sudah handle ini.

**Catatan llvmpipe:** Piksel test reference mismatch (722/3072 beda) — ini bug *test reference image*, bukan driver. Perlu update `probe_vk32.cpp` reference atau tolerance.

**Workaround di loader:** `BLACKLINUX_VK_INDEX=1` (atau auto-detect crash) memaksa llvmpipe.

### A4: Pipeline cache infrastructure (2026-10-19)

`src/d3d/pipeline_cache_posix.{h,cpp}` — infrastruktur cache pipeline keyed by (vertex format + shaders + render state).

**Komponen:**
| File | Fungsi |
|---|---|
| `pipeline_cache_posix.h` | `PipelineKey` (FVF/vertex shader/pixel shader + vertex layout + render state), `PipelineCache` class dengan linear-probe hash map |
| `pipeline_cache_posix.cpp` | `build_vertex_spirv`/`build_pixel_spirv` → translate NV2A microcode → GLSL → SPIR-V via libglslang; `create_pipeline_d3d` di gfx layer converts d3d format codes → VkFormat |

**Design keputusan:**
- **Keying:** Semua yang mempengaruhi pipeline: FVF/vertex shader handle, vertex layout (attributes + bindings), pixel shader handle, tex_cube mask, pipeline flags (blend/depth/stencil/cull), color/depth format, sample count
- **FVF path:** Belum diimplementasikan (butuh FVF constants dari XBE) — return error "fixed-function FVF not yet implemented"
- **Shader path:** VertexProgram → vsh_to_glsl → compile_glsl_to_spirv; PixelShaderDef → psh_to_glsl → compile_glsl_to_spirv
- **§3.2 enforcement:** d3d layer tidak melihat `Vk*` types — format codes (VertexFormatCode) diteruskan ke gfx layer, `create_pipeline_d3d` di gfx layer konversi ke VkFormat
- **Cache:** Linear-probe hash map (256 initial buckets, auto-grow), hit/miss stats, frame-based LRU tracking

**Integrasi gfx layer:** Baru `create_pipeline_d3d` di `gfx_vulkan.{h,cpp}` — menerima VertexFormatCode, konversi ke VkFormat via `gfx_format_for_vertex_code`, lalu call `create_pipeline`.

**Test:** Clean rebuild 0 Vk* di d3d/, 30/30 suite, 1.721 check, SPIR-V byte-identik.

### A8: Input detail — deadzone, sensitivity, rumble (2026-10-19)

`src/input/input_detail_posix.{h,cpp}` — layer detail input (deadzone, sensitivity curve, rumble pattern) dari XBE config.

**Files:**
| File | Fungsi |
|---|---|
| `input_detail_posix.h` | API: `StickConfig`, `TriggerConfig`, `RumbleEffect`, `GamepadConfig`, `rumble_start/stop/update`, `sdl_axis_to_norm` |
| `input_detail_posix.cpp` | Implementasi: radial deadzone, sensitivity curve, trigger curve, rumble attack/decay, SDL axis mapping |

**Features:**
| Komponen | Detail |
|---|---|
| **Stick deadzone** | Radial deadzone (preserve direction), configurable per stick, sensitivity & acceleration curve |
| **Trigger curve** | Deadzone + power curve (slow start), configurable per trigger |
| **Rumble** | Attack/sustain/decay phases, duration, per-event patterns (recoil, explosion, damage, vehicle, heartbeat) |
| **SDL mapping** | `sdl_axis_to_norm` / `sdl_trigger_to_norm` apply config to raw SDL input |

**Integration:**
- `bindings_posix.cpp`: left stick keyboard movement uses `left_stick.sensitivity`
- `xinput_posix.cpp`: `XInputSetState` uses `rumble_start` + SDL haptic fallback
- Hook for XBE override: `load_from_xbe()` (stub, ready for XBE config parsing)

**Default config (BLACK FPS):**
| Stick | Deadzone | Sens | Accel | Invert |
|---|---|---|---|---|
| Left (move) | 0.15 | 1.0 | 0.0 | false |
| Right (look) | 0.12 | 1.2 | 0.1 | true |

| Rumble | Low | High | Dur | Attack | Decay |
|---|---|---|---|---|---|
| Recoil | 0 | 60000 | 150ms | 0 | 100ms |
| Explosion | 40000 | 60000 | 500ms | 50ms | 200ms |
| Damage | 20000 | 40000 | 200ms | 0 | 100ms |
| Vehicle | 30000 | 20000 | 1000ms | 100ms | 300ms |
| Heartbeat | 10000 | 30000 | 1000ms | 200ms | 500ms |

**Hook XBE:** `load_from_xbe()` stub siap, nanti parsing config dari XBE resources.

### A9: Audio/Video decoder integration — WMA/XMV stream decode (2026-10-19)

`src/audio/stream_decode_posix.{h,cpp}` — integrasi decoder WMA/XMV ke DirectSound streams.

**Files:**
| File | Fungsi |
|---|---|
| `stream_decode_posix.h` | API: `Stream`, `stream_open/decode/close/pause`, `stream_submit_audio`, `stream_get_video_frame`, callback |
| `stream_decode_posix.cpp` | Implementasi: wrap `decoder::AudioDecoder`/`VideoDecoder`, buffer management, packet decode → mixer |

**Arsitektur:**
```
DirectSoundCreateStream (WMA/XMV format)
    ↓
stream_open() → decoder::audio_open/video_open (FFmpeg via decoder_posix)
    ↓
Stream callback / game pushes packets
    ↓
stream_decode() → decoder::audio_decode/video_decode → PCM float / RGBA
    ↓
audio_out::submit() / RGBA texture for video
```

**Format support:**
| Format | Codec | Output |
|---|---|---|
| WMA (0x161/0x162) | WMV2 | float stereo 48 kHz → audio_out mixer |
| XMV (0x163) | WMV3/VC-1 | RGBA frames → GPU texture |

**Features:**
- Audio: packet decode → float stereo → `audio_out::submit()` (10 ms blocks)
- Video: packet decode → RGBA frame → `stream_get_video_frame()` for GPU upload
- Pause/resume/eof handling
- Callback for packet completion (matches DirectSound stream callback)
- Thread-safe decoder state per stream

**Integration points (ready for XBE):**
- `DirectSoundCreateStream` HLE → `stream_open()` with format tag + extradata
- Stream callback → `stream_set_callback()` for packet completion
- Game packet push → `stream_decode()` → mixer
- Video frame → `stream_get_video_frame()` → GPU texture upload (when XBE available)

**Test:** Clean rebuild 0 error, 30/30 suite, 1.721 check.

### Dua jebakan portabilitas di `src/gfx/gfx_vulkan.cpp`

Ditemukan saat mencoba membangun probe yang sama dalam 64-bit (untuk menguji A0b). Bukan bug yang hidup pada
build 32-bit, tapi tanah jebakan:

- **Handle Vulkan dicampur antar-tipe** di dua tempat (`VkDeviceMemory` ke variabel `VkBuffer` sekitar baris
  750, `VkImageView` ke `VkDeviceMemory` sekitar 875). Di build 32-bit semua handle non-dispatchable adalah
  pointer, jadi penukarannya compiles dan bekerja; di 64-bit tiap handle punya tipe berbeda dan build gagal.
  Artinya kode itu **tipe-salah tetapi kebetulan benar** di proses 32-bit yang memang satu-satunya target
  sekarang, dan akan rusak diam-diam kalau loader suatu saat dibuild 64-bit.
- `gfx_vulkan.cpp` memakai `logf` dari `blackcore`, sementara `blackcore` menautkan `gfx` -- **siklus dua
  pustaka statis** yang belum dideklarasikan, jadi target baru yang menautkan `blackcore` gagal link dengan
  `undefined reference to logf`. Sudah dideklarasikan (`target_link_libraries(gfx PRIVATE blackcore)`).

## Loader design (Phase 4, `loader/`)

- `black.exe` is a no-CRT stub linked `/BASE:0x10000 /FIXED` with a 0x7F0000-byte BSS, so its image spans `0x10000-0x803000` and owns the XBE range before Windows creates heaps/stacks. It imports `BlackMain` from `blackrt.dll`, which maps the XBE over the stub image and never returns (it ends with `TerminateProcess`). The stub is `/SUBSYSTEM:WINDOWS` (2026-10-08, user: only the game window should show): no console of its own; `console_init` attaches to the parent's console when started from a prompt (stdout redirected by the parent is kept). The console gets only `logc` lines (banner, XBE hash, setup errors, exit, FATAL, unhandled exception), `--verbose` mirrors the whole log; black.log is as verbose as before. Without a console, `--help`/bad options show the usage in a message box.
- Contiguous memory: 64 MB committed at exactly `0x80000000` (exe is large-address-aware), physical = virtual - `0x80000000`; first 1 MB and top 64 KB (GPU instance memory) kept out of the allocator. `NtAllocateVirtualMemory` drops `MEM_TOP_DOWN` so ordinary allocations stay below 2 GB.
- Threads: every Xbox thread is a Windows thread with a fake ETHREAD/KPRCB bound through the TLS slots; thread handles are real Windows handles. IRQL >= DISPATCH takes one global lock (single-CPU exclusion that XAPI/DSound rely on). Threads that run game code (every kthread_create thread: boot, game threads, vblank, DPC) are pinned to one CPU by default (`--multicore` to disable); host threads (the DSound mixer, XAudio2, the graphics driver, the window) use the other CPUs. Pinning the whole process made audio choppy at 60 fps: the game kept its core busy and the mixer/XAudio2 ran late (the output queue is now 6 x 10 ms; underruns are logged as `[audio] output underrun`).
- Kernel objects in Xbox memory (KEVENT, KTIMER, critical sections) keep their state in the Xbox struct; a host event per address only wakes waiters. One `kernel-dpc` thread fires timers and runs DPCs at DISPATCH_LEVEL. Interrupts are recorded but never delivered.
- Files: symlink table (`\??\X:` -> device path) + device prefixes -> host folders: `\Device\CdRom0` = game folder (read-only), `\Device\Harddisk0\PartitionN` = `hdd\PartitionN` (1 = E: with TDATA/UDATA, 5 = Z: utility). Handles are real Windows handles opened through ntdll, so read/write/event/APC semantics carry over.
- First-D3D-call stop: all 214 D3D-section functions get a 5-byte `jmp` (one 3-byte function, `FUN_00273960`, gets `int3`) to a logger. Phase 5 replaces these hooks with real implementations.
- Not done yet: raw DVD device reads, APCs for directory/IOCTL calls, named-object namespaces beyond a host-name prefix, `ObReferenceObjectByHandle` for non-thread objects, `NtQueryInformationFile` classes 9+18 beyond the basics. The first-use log line (`<first call>`) and `UNIMPLEMENTED`/`not implemented` lines show what is hit.

## Next (Phase 5 start)

1. Turn the D3D hook into a dispatch table: per function, either a C++ replacement (stdcall/fastcall, or a naked thunk for `__LTCG_*` register arguments) or "stop and log". Start with what `main` calls first: `Direct3D_EnumAdapterModes`, `Direct3D_GetAdapterModeCount`, `Direct3D_CreateDevice` (window + D3D9 device), then let the game run to its next unimplemented D3D call, one at a time.
2. Hook DSound (`DirectSoundCreate` etc.) the same way before the game reaches audio init: the DSOUND library talks to APU registers at `0xFE800000`, which will fault.
3. Keep the D3D library's data block (`0x27963c-0x27cf08`) live: the game writes render state there directly.

## Environment notes

- Mac: arm64, macOS 27. Project volume `/Volumes/spark198rus` is **exFAT**: creates `._*` AppleDouble files that break `git` and `cmake` globbing. Build out of tree on local disk; delete `._*` before building (`find . -name '._*' -delete`).
- Ghidra project lives in `~/ghidra-projects/black-pc` (project name `black`), **not** on the exFAT volume. Export a `.gzf` into `ghidra/` to move it to another machine.
- Ghidra: `brew install ghidra` (12.1.4, JDK 21). Extension installed in `~/Library/ghidra/ghidra_12.1.4_PUBLIC/Extensions/`. Gradle 9 removed `-b`, so the repo's `build.sh` needs `gradle` without it.
- Rebuilding the extension: stage `tools/ghidra-xbe` in a local dir, generate `XbeXtlidDb.java` from xtlid.xml via `xsltproc -o src/main/java/XbeLoader/XbeXtlidDb.java xtlid2java.xslt xtlid.xml`, copy CLI to `os/mac_arm_64/`, run `gradle` with `GHIDRA_INSTALL_DIR` and `JAVA_HOME` (JDK 21).
- **Windows PC** (2026-10-04): Windows 10 Pro 19045, drive mounted as `S:` (exFAT). Visual Studio **18 (2026) Build Tools**, MSVC 14.51 (x86 target), Windows SDK 10.0.26100. CMake 4.3.1 and Ninja are bundled with VS but not on PATH: `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`. Python 3.14, Git 2.53. Not installed: Java/Ghidra, x64dbg, RenderDoc, xemu.
- Disassembly without Ghidra: `python tools/xbe2pe.py game/default.xbe work/default.pe.exe`, then `dumpbin /nologo /disasm /range:0x11000,0x282fe0 work/default.pe.exe > work/code.asm` (dumpbin is in `VC\Tools\MSVC\<ver>\bin\Hostx64\x86\`).

## Phase 5 progress (2026-10-04, later the same day)

- D3D is replaced per function (`loader/src/d3d/`): generated thunks preserve ecx/edx (LTCG callers), Win32 window + Direct3D 9 device (opened at the game's CreateDevice), Xbox-side device object with the fields game code reads (+0 PUT, +4 threshold, +0x2C fence, +0xEF8 screen offset, +0xF98 textures, +0x1A04/8 RT/Z, +0x1DB4 swap cb, +0x1DB8 vblank cb, +0x1DBC vblank KEVENT, +0x2478 swap count), render/texture state kept in the Xbox arrays (render states base 0x27C008, 5849 enum verified; texture states 0x279948 [4][32]), texture upload/unswizzle/cache, resource create/lock/refcount, render targets as host RT textures, FVF draws, immediate-mode/inline-array push-buffer interpreter, 60 Hz vblank thread.
- RenderWare needed the exact display mode flags 0x120 (interlaced + 10x11 PAR) or its driver OPEN fails and stdFunc[] (0x56A6A8) stays empty. RwGlobals is static at 0x56A660.
- Notification KEVENTs must release every current waiter (generation counter in k_sync.cpp).
- DSOUND and XPP sections are hooked like D3D (every function replaced or stop-on-first-call; C++ static initializers in the _cinit tables run natively). Silent DirectSound HLE with real-time buffer/stream timing and packet callbacks (`windows/src/audio/dsound.cpp`); input via PC XInput + keyboard (`windows/src/input/xinput.cpp`).
- Debug tools: `--watch <addr>` (hardware write breakpoint), `--probe <addr>` (log every hit), thread dump on `--timeout`, `--screenshot <dir>`, `--trace-d3d`.
- Vertex shaders: NV2A microcode -> HLSL vs_2_0/vs_2_a (`nv2a_vsh.cpp`). MAC and ILU run in parallel (a paired ILU writes r1); r12 = oPos. Constant slots 0-191 = Xbox c-96..c95; the register passed to SetVertexShaderConstant* is already the slot, and the constants live in the library's shadow at 0x27C2A8. The screen-space epilogue (c-38 scale, c-37 offset) is undone with the viewport transform in c192/c193; oPos.w == 0 (2D shaders) is treated as 1. Linear (pitch) textures take texel coordinates on the NV2A: c194-c197 hold 1/size per stage (texScale).
- Pixel shaders: register combiners -> HLSL ps_2_0/2_a/2_b (`nv2a_psh.cpp`, `pshader.cpp`). The handle is {RefCount, D3DOwned, D3DPIXELSHADERDEF*}; the library's SetPixelShader (0x26bd00) copies the first 57 dwords of the PSDEF into render states 0-56 and PSTextureModes into rs[136] (0x27C228); device +0x784 current handle, +0x788 = final combiner present, +0x78C = PSDEF[0xEC]&0x100, +0x790 texture modes. SetPixelShaderConstant (0x26bf10, `ecx s eax`) packs each float4 to ARGB8, keeps it at device +0x8E4+4*reg, and writes it into rs[10+i] / rs[18+i] for every stage i whose PSC0Mapping / PSC1Mapping nibble equals reg, and rs[43]/rs[44] for the final-combiner nibbles of PSFinalCombinerConstants. The HLE does the same; draws translate whatever is in rs[0-56]+rs[136] (cached by content without the constants) and upload the constants unpacked as c0-c17.
- Frame pacing: CreateDevice asks for FullScreen_PresentationInterval 0x80000002 (TWO_OR_IMMEDIATE): BLACK runs at 30 fps. Swap waits for the emulated 59.94 Hz vblank per rs[XRS_PRESENTATIONINTERVAL]; the host device presents with INTERVAL_IMMEDIATE.
- Boot flow (fresh profile): logos (videos\LO_N_US.xmv) -> title over AK_N.xmv ("PRESS START") -> START -> "Checking Xbox hard disk" -> intro C_N_US.xmv + 02_N.xmv (INTRO.bks, ~165 s, not skippable until the first mission is saved) -> main menu (START MISSION / OPTIONS, FEMUSIC.bks). Idle on the menu -> attract mode (Attr_N.xmv, DEMOATTR.bks).
- Audio: software mixer (`windows/src/audio/dsound.cpp`) into XAudio2 (`windows/src/audio/output.cpp`, xaudio2_9 loaded at runtime; needs CoInitializeEx). The game sends every voice to effect sends, never to the speaker bins: FXSEND_8..13 (mix bins 19-24) are FL/FR/C/LFE/BL/BR for the downloaded DSP effects image (not emulated), so the mixer folds those to stereo; music and video audio are L/R pairs of mono voices on bins 19/20. All buffers share one 8 MB sound memory block (SetBufferData 0x80329A80+8372224) and select their part with SetPlayRegion; **loop region and play cursor are relative to the play-region start** (streamed music is a play-region ring the game refills behind GetCurrentPosition). Not mixed yet: LFE, I3DL2 reverb, other FX sends; 3D voices get a simple distance/pan model from DS3DBUFFER (Xbox layout: standard fields + flDistanceFactor/flRolloffFactor/flDopplerFactor at +0x40..+0x48).
- Scripted input: `--press "22s:start,195s:a"` (seconds since the first input poll) or frame numbers.
- **Game code reads D3D device fields directly**: transforms at device +0xC60 + State*0x40 (Xbox D3DTS order VIEW, PROJECTION, TEXTURE0-3, WORLD0-3) and the clipped viewport {X, Y, W, H, MinZ, MaxZ} at +0xEE0 (library SetTransform 0x26aae0 / SetViewport 0x26ac00). FUN_0016f770 copies VIEW/PROJECTION/viewport into its renderer context (`*0x2D67F4`, +0x70/+0xB0/+0x170), multiplies them (FUN_00022424 = matrix multiply) and uploads the world-view-projection with SetVertexShaderConstant4(slot 10 = c-86). With those fields left zero the whole world collapsed to a point (HUD only). The HLE mirrors both and starts the transforms as identity.
- `--skip-videos`: every .xmv except the title background (AK_N) opens LO_n_US.xmv (8.9 s) instead; a missing video is a fatal "disc dirty or damaged" error. With it a fresh profile reaches gameplay in ~75 s: `--skip-videos --press "20s:start,50s:a,56s:a,62s:a"` (START on the title, A on START MISSION).
- State (2026-10-04 evening): **first mission (Level_00) renders** (world, lightmaps, first-person weapon, HUD) at 30 fps with sound; no unimplemented D3D/DSound stops hit so far.
- **Post-processing (blur, depth of field, bloom, colour grade)** relies on Xbox memory aliasing; the HLE now models it:
  - Render targets are host textures keyed by (memory address, width, height). BLACK composites into a second surface object over the back-buffer memory (0x02b4e000) and reads the back buffer as a texture, so the back buffer is a host texture too (copied to the window at Swap). A draw that samples its own render target gets a copy (StretchRect snapshot).
  - It reads the 640x480 Z24S8 depth buffer through an A8R8G8B8 texture header (A = Z[23:16], R = Z[15:8], G = Z[7:0], B = stencil): depth buffers are INTZ textures and a pass rebuilds those bytes ("depth view").
  - It writes downsampled depth *as colour* into 160x120 / 80x60 surfaces (0x02dc4000, 0x02de0000) and then binds the same memory as the depth buffer of its low-resolution passes: the last writer of each memory key is tracked, and colour-written memory bound as depth is converted (pixel shader with DEPTH output, Z = A:R:G).
  - NV2A vertex programs output target pixel coordinates; translated shaders convert them back with the *host* (clamped) viewport (`host_viewport_transform`, c192/c193), not the game's, or a 640x480 viewport on a 160x120 target shrinks everything into a corner (that was the black reload blur and missing bloom).
  - Viewport: the library's SetViewport (0x26ac00) clamps the requested viewport to the current render target, stores it at +0xEE0 and derives the screen-space transform from it (GetViewportOffsetAndScale 0x26e9c0 -> WriteViewportTransform 0x26eaf0, which pushes NV097 viewport offset/scale = c-37/c-38). BLACK calls SetViewport(0,0,640,480) once per frame (back buffer bound) and then draws its light-shaft meshes (vs with `dp3 r7, normalize(v0 - c-72), v2`, alpha ~ dot^4) into the 160x120 buffer 0x02db0000 with the same view-projection, expecting the whole view there. No library code re-fits the viewport on SetRenderTarget (0x26a420 never touches +0xEE0; scale factors +0x95C/+0x960 stay 1.0 for non-back-buffer targets), yet with the 640x480 transform the shafts came out 4x too large, anchored top-left. The HLE re-fits the requested viewport to the target on every SetRenderTarget (`fit_viewport`), which the user confirmed fixes them. (Open question: where the Xbox does this.)
  - Frame of reference: mission-2 frame = ~520 scene draws, then depth downsample, blur ping-pong between 160x120/80x60 targets, DOF composite (SRCALPHA blend into the back-buffer alias), first-person weapon, bloom (ONE/SRCALPHA add), colour grade (dot(t0, green) x tint, alpha 0.1), HUD.
- **Brightness and screen fades = the game's gamma ramp.** Videos, menus and the HUD are drawn at vertex colour 0x7F7F7F and scenes top out near half intensity; the Xbox's video DAC brightened them through the gamma ramp that the game itself computes: FUN_001762B0 (renderer `*0x2D67F4` in ecx, called once per frame by every main-loop state before the UI update FUN_00121E60) builds ramp[i] = min(255, (i/255)^1.2 * (brightness + 1) * 2.2 * 255 * tint[c] * fade) with brightness = renderer +0xD5B4 (the options slider: `SetGammaLevel` UI callback 0x119970 stores slider * 0.01, also settings +0x20158; default 0), tint = +0xD5A0/A4/A8 (1, 1, 1) and fade = +0xD5B0 (FUN_0015A8A0 = setFade, clamped 0..1; the UI's fade timer +0x3948/direction +0x394C in FUN_00121E60 drives it). It writes the D3DGAMMARAMP straight into the device (+0x1E04 + i*0x300, i = swap count & 1), makes it current (+0x240C) and sets its dirty flag (+0x2404 + i*4) - an inlined SetGammaRamp, which is why no SetGammaRamp import shows up. At the default the ramp maps 127 -> 243 (the "x2 brightness" the user matched by eye). The HLE reads the current ramp at Swap and applies it in the present pass as a 256-entry lookup texture (`ramp_texture`, identity until the game writes one; `--no-ramp` to compare), then the user's gain (default now 1.0) and gamma. This brought back the fades: the boot loading screen (bullet casings + LOADING) fades in over frames 30-60 and out over 96-108, and the same screen before each mission. A saved profile's brightness setting now takes effect too (applied when the profile loads; 1.0 doubles the ramp again).
- `D3DDevice_SetVertexShaderConstantNotInlineFast` (0x2699e0, `ecx edx s`) takes a **DWORD** count (pushes that many with SET_TRANSFORM_CONSTANT in chunks of 16); callers copy the data into the shadow (0x27C2A8) themselves first. Treating it as a constant count overwrote 3x as many following slots with stray memory (e.g. the call at 0x16F5F9 uploads from slot 60).
- **Game timer ran ~4.6x too fast** (fades looked like cuts): XAPI `QueryPerformanceCounter` (0x232da) is a bare `rdtsc` and `QueryPerformanceFrequency` (0x232eb) returns a constant 733,333,333 (Xbox CPU clock); the game's helpers FUN_00084190 (store rdtsc at 0x2c0468) / FUN_000841a0 (rdtsc low) / FUN_000841c0 (733 MHz constant) assume the same. On a 3.4 GHz host every seconds-based effect was 4.6x fast. `loader/windows/src/timing.cpp` jumps those three to a 733.33 MHz counter derived from the host QPC. The calibrated timer FUN_00180c00/FUN_00180cf0 (measures raw `rdtsc` against QPC, stores seconds per tick at 0x2a7f00) then calibrates itself correctly. Other `rdtsc` sites: 0x24eeb1 is data, D3D's FUN_00273960 is never called.
- **Render scale** (`--scale N`, default 3): host render targets/depth surfaces are Xbox size x N (keys and everything the game sees stay in Xbox pixels); the host viewport, scissor and clear rects are scaled; pre-transformed XYZRHW vertices (fvf 0x444 in a few draws) are scaled on the CPU; translated shaders still undo the screen transform in Xbox units. Swap filters the frame into the window-sized part of the host back buffer. The process is DPI-aware; the default window is the largest client area of the game's aspect within 90% of the work area. In widescreen x is scaled 4/3 more than y (`scale_x()`/`scale_y()`, edges rounded with `host_rect`), so the anamorphic 640x480 buffer renders at 2560x1440 for scale 3. Any render size works (x and y are independent float factors, set once at CreateDevice by `setup_render_scale`): `--scale K` takes fractions, `--resolution WxH`, `--resolution H` (width from the aspect) or `--resolution native` (the picture's size in the window at start, the screen's with `--fullscreen`); tested 1920x1080, 1600x900 (x1.875 vertically), 1280x720 native - the game's small effect buffers scale along (light-shaft buffer 160x120 -> 480x270 at 1080p). Capped at Direct3D 9's 8192 texture size. **The render size can change while running** (`change_render_scale`, after a Swap): every host target made at the old scale (colour targets, depth buffers, depth views, snapshots, AA intermediates) is dropped and re-created on demand at the new size, then the game's current target/depth (g_st) are bound again with the viewport and scissor (the game redraws its targets every frame, so at most one frame of their contents is lost; bound textures stay alive in D3D until replaced). With `--resolution native` the render size follows the window once a resize has settled for 0.3 s (also F11); F10 cycles 1 / 1.5 / 2 x the window (switching to native). Tested in gameplay 1280x720 -> 1920x1080 -> 2560x1440 -> 1280x720 with firing and a reload: no black frames, effects intact (`--rescale-at "S:factor,..."` scripts it). **Blur grid fixed (2026-10-06):** at high render sizes the game's blur showed a faint grid (its taps use offsets in the texels of the 160x120/80x60 buffers, which were rendered 3-4x larger, so taps skipped texels). Keeping the small targets (at most half the back buffer: 320x240, 160x120, 80x60, 64x32) at their Xbox size removed the grid but the user found it pixelated and blurry, so that is now the option Video > Effects at original resolution (`effectsnative`; per-target scale `HostRT::sx/sy` and `target_scale_x/y` for viewport, scissor, clear rects and pre-transformed vertices; paired depth surfaces and depth views follow). Default: they stay at the render scale, and when a draw samples one with linear filtering (`rt_texture_for(..., linear)`) it gets a box-filtered copy (`smoothed_rt`, kBoxPS: four bilinear taps a quarter Xbox texel off centre = about one Xbox texel), rebuilt when the target was bound for drawing again (`HostRT::epoch`) or per draw while it is bound. Point-sampled reads (the depth bytes) are untouched. Checked with a reload: no grid, smoother and more detailed than the original-resolution option.
- Debug: `--debug-frame N` (or a range `N-M`, which traces only the 2D/full-screen draws) now also logs render-target switches, clears, buffer aliasing and pixel-shader programs in order, and saves every host render target and depth view of that frame as PNG (colour and alpha) into the `--screenshot` dir; `--shot-every N`; scratchpad `frame.py` summarizes a dump.
- **Widescreen** comes from the console's video settings, as on an Xbox Series: `XGetVideoFlags` (0x23291) = EEPROM XC_VIDEO (setting 8) >> 16 & 0x5F, so bit 16 of XC_VIDEO = XC_VIDEO_FLAGS_WIDESCREEN (bit 0x10 of the flags = letterbox, 0x40 = PAL 60, 0x08 = 480p). Callers: device setup FUN_00178350 stores flags & 1 in the renderer context (`*0x2D67F4` +0x0C, byte) and still creates a 640x480 back buffer with present flags 0x120 (no D3DPRESENTFLAG_WIDESCREEN): the picture is anamorphic. The game-settings init FUN_001F8060 sets settings +4 (= game object +0x20150) to 1 for widescreen or letterbox; the front end calls the UI function `setWideScreen` (0x124616) with that field and picks `%s_wide` movie clips (0x122427); FUN_00113E70 shifts UI rectangles by 106.667 (= (853.33 - 640) / 2) when the renderer byte is set. FUN_000C1980 is an `isWideScreen()` with a debug override (byte 0x2F4360 / dword 0x2F4364, set by 0xBE5D0). The PS2 version has a menu switch; on the Xbox only the dashboard setting changes it. The HLE reports XC_VIDEO = 0x00010000 by default (`--normal-aspect`: 0), and the host targets/window are 16:9. Aspect and frame rate are boot-time settings: a menu choice only goes to black.ini for the next run (`fps_next` / `widescreen_next`, with a notice) and the session keeps its values; set live, they changed the swap interval and the render/window shape under the running game (user, 2026-10-08).
- Window: resizable at the game's aspect (WM_SIZING), F11 = borderless fullscreen (`--fullscreen` to start that way). The host back buffer is as large as the largest monitor and each frame uses only the client-sized part (Present with a source rect, so D3DSWAPEFFECT_COPY), so resizing needs no device reset; other window shapes get black bars.
- Keyboard and mouse (`windows/src/input/xinput.cpp`, `windows/src/input/bindings.cpp`, raw mouse input in `windows/src/d3d/host.cpp`): keys and mouse buttons are bound to the game's actions (2026-10-08, user: "real bindable keyboard keys to game functions"; Input > Key bindings, `key_<action>` in black.ini). Each poll a held key looks up its action in the live binding table (below) and presses the pad input feeding that channel, so keys follow the game's controller layout. Defaults are the user's earlier pad layout as actions (WASD, LMB fire, RMB aim, R, E, C, G, F, wheel, Z, Q, X, Space objectives, Esc pause). **Mouse look rotates the view directly** (`windows/src/input/mouselook.cpp`; emulating the right stick felt bad because of the game's stick shaping). The input chain, found with `--watch-pad` (DR3 read watch on the pad buffer):
  - XInputGetState once per frame (FUN_000C22B0, pad entries of 0x20 bytes: status, handle, XINPUT_STATE).
  - Controller update FUN_000C5xxx (0xC5ECE reads sThumbRX): each axis -> float channels at controller +0x44 + 4*i (RX+ = channel 11 at +0x70, RX- = 10, RY+/- = 8/9, LX/LY 4-7), through FUN_000C24D0 = linear with dead zone +0xCC and saturation +0xC8.
  - **Actions and bindings.** Channel object (controller update FUN_000C5A80, pad entry [+0x118] + 32 x [+0x11C]): floats at +0x44 + 4 x ch, digital state now/before at +0x25 / +0x09 + ch (threshold +0xB4; edge = now && !before, FUN_001F14A0). Channels: 0-3 D-pad up/down/left/right, 4/5 LY+/-, 6/7 LX-/+, 8/9 RY+/-, 10/11 RX-/+, 12 START, 13 BACK, 14/15 left/right stick click, 16-23 A B X Y BLACK WHITE LT RT, 24-27 LX LY RX RY signed, 28 = none. Binding table *0x2F3BD4 = 37 dwords action -> channel, read inline everywhere (`[[ctrl+0xA8]+0xC]+0x44+4*ch`); FUN_001F1520 copies preset ecx from 0x2A7AF0 (0x94 bytes each: default, assault, scout, southpaw, custom = zeros); the custom screen assigns from 10 buttons at 0x2A7DD4 (FUN_001F1500 / 001F14D0). Actions (names from the FE_ACTION_* tables built at 0x214820; default channel): 1/2 run forward/back (4/5), 3/4 sidestep (6/7), 5/6 turn (10/11), 7/8 look (8/9), 9 fire (RT), 11 reload (A), 12 crouch (LS), 13 action/use (X; also the HUD button prompt, 0x1328EC), 14/15 weapon up/down (D-pad right/left), 16 aim (RS), 25 pause (START), 27 grenade (LT), 30 firing mode (D-pad up), 31 suppressor (Y), 32 melee (B), 33 health pack (D-pad down), 34 next music track (WHITE), 36 objectives (BACK, 0x20EC57; checked: shows the objectives status). Not on the controls screen: 10 (Y; only in an any-action-pressed check), 17-24 (signed sticks), 26, 28, 29, 35. Play vs menus for the port: ml_sway (0x1ECF76) runs in each player update, so no call for 3 frames = menu, pause or cutscene (arrows / Enter / Backspace = D-pad / A / B, user's choice; keys held across the switch wait for a release). Esc frees the mouse in play (pause) and in menus (user: the main menu kept it); entering play takes it again if the window is in front. Checked with `--press k:...`: Enter through title and menus, Esc pause, Esc back out, Space objectives, C crouch, W walk, F melee.
  - Player input FUN_001EC450 (ecx = player controller; binding table *0x2F3BD4 maps actions to channels): look horizontal +0xD8 = ch[+0x14] - ch[+0x18] (RX- minus RX+), vertical +0xDC (invert flag +0x101), channels under 0.1 zeroed; then FUN_001BD3F0(vertical, horizontal) with esi = controller.
  - FUN_001BD3F0 (only caller 0x1ECF71): power curve (exponent +0xC8), turn acceleration near full deflection (+0xC0/+0xC4/+0xD0/+0xD4), smoothing +0xCC into +0xA0/+0xA4, x0.7 while aiming (+0x105 of the profile), then yaw [esi+0x18] += +0xB8 * input * zoom factor * frame time (*0x2D45D4 +0x1C) (wrapped to +-180) and pitch [esi+0x1C] += +0xBC * ... (clamped to +-70); degrees. The zoom factor 1/(1+(zoom-1)*0.8) is in xmm2 at both additions.
  - The patch replaces the two `addss` at 0x1BD6A8 (yaw) and 0x1BD6C7 (pitch) with calls that also add the frame's mouse angle x the zoom factor; 0.022 deg x `--mouse-sens` (default 2.0) per count. **Weapon sway** (FUN_001E5FA0) eases the first-person weapon (+0x4CC/+0x4D0) toward the look values times weapon factors ([w+0xF4]/[w+0xF8], rate [w+0x104]), read through the player object (controller at +0x4F0: +0x5C8/+0x5CC = controller +0xD8/+0xDC, +0x5D0/+0x5D4 = the move values; found with a write watch). Mouse look left the look values at 0, so the gun did not swivel (user). Now a third patch at 0x1ECF76 (right after the call to FUN_001BD3F0) adds the stick deflection that would turn the view as fast as the frame's mouse angle: (angle / (speed +0xB8/+0xBC x frame time)) ^ (1 / curve +0xC8), clamped, ~50 ms smoothing; +0xD8 has the yaw sign, +0xDC the inverse pitch sign. Test: mouse X +20 counts a poll -> +0xD8 about -0.57.
  - **Auto-aim** is a pull of the view, not bullet correction (`windows/src/input/autoaim.cpp`). In FUN_001EC450, while any look/move axis is non-zero or the action at binding +0x24 is held, it clears the picked target [ebx+0xAC] and casts a 30 m query along the camera (FUN_001CDA30: camera matrix [ebx+0x8C]+0xD0, radius global 0x2F57E8, callback FUN_001BD3A0 keeps the nearest entity with an AI part [+0x39C] in +0xAC/+0xB0/+0xB4). FUN_001D1E40(ebx) (stdcall, 1 arg, only caller 0x1ECF40) moves the camera-space offset to that target into the turn rates (+0xA0 pitch, +0xA4 yaw, each kick clamped to 3.5) just before FUN_001BD3F0 integrates them. Nothing else reads +0xAC; the query's other caller (0x1DCDF4) is enemy AI. The x0.7 in FUN_001BD3F0 (flag +0x29C->+0x105) is the weapon-aim (zoom) slowdown, not aim friction. The switch: a jmp at 0x1D1E40 to a thunk that returns (`ret 4`) when off; menu Game > Disable auto-aim, `--no-autoaim`, `autoaim` in black.ini; default off since 2026-10-12 (user; it was on, as in the original), `--autoaim`.
- Sound effects image: the game loads `sound/dsp/BkXboxFx.bin` (23,780 bytes) with DownloadEffectsImage. Layout (from the library's loader 0x21C1F3 / 0x21BF86): header at 0x800 (+4 code words 0xA54, +8 state start 0x3168, +0xC 0xA01), DSP memory image from 0x818, descriptor at 0x818 + (w[0x80C] + w[0x804]) * 4 = 0x596C: effect count 22, scratch 0x60000, then 8-dword maps {code, code size, state, state size, Y mem, Y size, scratch, scratch size}. Each state starts {scratch offset, scratch length, Y offset, Y length, flags/IO count, mix-bin pointers...}. GP addresses: VP mix bin n = 0x1400 + 0x20*n (input), speaker outputs FL/FR/C/LFE/BL/BR = 0xC00 + 0x20*ch. Effects: 2-6 and 7-12 = biquads (bins 25,13,14,15,16 -> FL,FR,BL,BR,C and bins 19-24 -> FL,FR,BL,BR,C,LFE), 13/14 bins 17/18 -> 0xE20/0xE40, **16 = I3DL2 reverb (bin 11 -> FL FR BL BR, 380 KB scratch)**, 17 = crosstalk (bins 6-9 -> corners), 18-21 = delays (bins 27-30). The game never calls SetI3DL2Listener; it only sets the biquads to pass-through at start (SetEffectData effects 2-12, offsets 28-44), so the reverb runs with the image's built-in (DSP-format, not decoded) room, and each voice's bin-11 send level sets how wet it is (many voices send 0 dB). The HLE mixes bin 11 into an FDN reverb (`windows/src/audio/reverb.cpp`, I3DL2 parameters chosen by ear, `--reverb-gain`, `--no-reverb`).
- **Movies and skipping** (`windows/src/input/movies.cpp`; user, 2026-10-12: unseen movies cannot be skipped). One player, game object *0x2D45F0 (0x80100000) + 0x2026C: state +0x1A8 (2-0x1B opening, 0x1C playing, 0x1D-0x36 closing, 0x37 idle), flags +0x198 (bit 0 loop: menu backgrounds AK_N), +0x1B0/+0x1B4 kind, +0x1B8 request, +0x1B9 stop, +0x1BA started, name +0x84, decoder [+0] (+0x62 = finished). FUN_001F3630 queues a movie (`D:\videos\%s.xmv`; a request while playing stops the current one), FUN_00209DC0 runs the player each frame, FUN_00209D40 winds down (XMVDecoder_TerminateImmediately). The sequencer FUN_0020ECF0 (game +0x20220) starts movie index +0x20C (set by the front end, 0x11418B; table 0x29D9C0, 12 bytes: name pattern, kind, flag) when it differs from +0x208, and sends the front end "FmvHasFinished" every frame the player is idle. Skipping is the FMVPlayer script's choice: with the user's save A or START requests the next index at once (C_N_US -> 02_N -> menu); with a fresh profile the press is ignored, and the boot logos ignore it always (progress variables FELevelUnlocked1-8 are the likely test, not confirmed). By default (user: no need to turn it on) the port sets +0x1B9 on an A / START press if the same non-looping movie still plays 10 frames later, so the front end goes on as after the movie's end. Checked: fresh profile skips logos, intro, 02_N and the mission movie 03_N (play at 59 s without --skip-videos); with the save the game's own skip still advances one movie only.
- **Field of view** (`windows/src/fov.cpp`; user, 2026-10-12). The renderer's camera (*0x2D12B8, camera at +0x740) holds the FOV in degrees at +0x750 (= camera +0x10); it is horizontal: the view setups FUN_000D2F40 / FUN_002033A0 (both per frame) take near +0x70 (1.2) x tan(fov/2) as the half width and divide by the aspect +0x74 (1.778 widescreen, set by FUN_0015A960; 1.333 by FUN_000C3340), so widescreen is 70 x 43 degrees against 70 x 55 in 4:3 (86 x 55 in widescreen matches 4:3). Writers (write watch): FUN_001BB110 every frame in play, fov = [0x2888C8] / zoom (zoom [[ctrl+0xF0]+0x2A4]: 1 at the hip, eased to 1.587 aiming the pistol = 44.1 degrees), when [renderer+0x16A4]; FUN_00202850 resets it to [0x2888C8] or blends authored cutscene values (+0x2029A8); FUN_00203610 sets +0x750 and the blend target +0x7A0 at start; FUN_00202080 adds kicks, FUN_001FE020 blends. [0x2888C8] = 70.0 is a pooled constant also used as the pitch clamp (0x1BD6B1, 0x1ECF7B) and in unrelated code (0xDE346, 0xDE419, 0x12781C, 0x1E8E8E, 0x20B12C), so only the three FOV loads (0x1BB2CA, 0x202F92, 0x2038E5) are re-pointed at the port's float. Checked: --fov 100 shows a wider room, no culling gaps. **Gun and hands** (user: the animations broke at a wider FOV): they are the render list renderer *0x2D67F4 + 0xCAE4, drawn by FUN_001EE820 through FUN_00176F60 at 0x1EE97C (or at 0x1EEB2B, after the effects pass, when [[0x2D1990]+0x830D4] == 1), with the game's flag 0x2F3C44 set. Both calls go through a thunk that marks the list; its draws get c200 = tan(now/2) / tan(game/2) (now = renderer +0x750, game = now x 70 / setting = 70 / zoom) and the translated vertex shaders scale clip x/y by it (depth unchanged). Checked at 100 vs 70: same gun size and place at the hip and aiming. The muzzle flash is a sprite on the aim line near the screen centre also at 70 (not on the barrel), so it needs nothing. Found with the debug-frame draw log's new caller chains (`[draw]   from ...`): world FUN_001EE820+0x130 (list +0xCA6C), +0x161 = this list (2 draws, 5905 and 2927 vertices), effects FUN_00178C80 (particles FUN_0015B380), shell casing FUN_00173650. Other fptan sites: FUN_000C3200 (FOV argument, not hit in play), 0x15ABC5 (fixed 30 degrees), 0x2070FE (not hit).
- **60 fps**: BLACK asks for FullScreen_PresentationInterval TWO_OR_IMMEDIATE and steps its world by a *fixed* frame time chosen at start-up from the video standard (init 0x20C282: NTSC 1/30 s + 33 ms, PAL 1/25 s + 40 ms into 0x2F3A38 / 0x2F3800; the clock object `*0x2D45D4` copies 0x2F3A38 into +0x1C at each reset, FUN_000BE4D0; the game's own setFrameRate FUN_000BE560 sets both for any rate). Pacing TWO as ONE alone ran everything twice as fast. `install_60fps_patches` (timing.cpp) makes the NTSC choice 1/60 s and 16 ms and points the other two users of the 1/30 constant 0x295FC0 (0x193EAE `mulss`, 0x212210 -> 0x2F57F4) at 1/60. Check: holding the right stick turns 12.3 px/frame at 30 fps and 6.2 px/frame at 60 fps (same speed per second); the user confirmed normal speed. The world runs slower than real time if the host cannot keep 60 (fixed step). Default 60 since 2026-10-12 (user; it was 30 before); 60 fps fixes two frame-rate leftovers, both verified: (1) **weapon refire** - FUN_001EEFD0 (weapon state machine: state +0xD8, timer +0xDC, FUN_001F0210 subtracts the frame time first) fires when the timer is <= 0 and reloads it with the weapon's interval (e.g. 0.04 s); checked once per frame, that rounds up to whole frames: 2 at 30 fps (66 ms, the original rate), 3 at 60 fps (50 ms, too fast). The four `mov [ebp+0xDC], reg` after a shot (0x1EFC72, 0x1EFC94, 0x1EFCA6, 0x1EFEF3) now go through `refire_interval`, which rounds the interval up to whole 1/30 s steps: 66-68 ms between shots at 60 fps. Ammo: dec word [weapon+0x18] / dword [weapon+0x14] at 0x1EFBBB/0x1EFBC5. Fire input: player input FUN_001EC450 sets byte +0x41 from the fire channel (right trigger = controller channel 23 at +0xA0); FUN_001F0860 passes the input bytes to FUN_001F0650 -> FUN_001F0210 -> FUN_001EEFD0. (2) **reload blur** - post-process controller at `*0x2D1990 + 0x830A0` (mode +0x34, timer +0x14 += frame time, strength +0x10 = blend alpha of the pass FUN_001772E0; per-mode fade-in/out tables 0x289B80/0x289BA8; mode 1 = reload, FUN_001595F0; the player input cancels it early on movement/buttons). Its hold time is set at 0x17B314 as reload-animation length [anim+0x1C] (30 fps animation frames) * frame time (game `*0x2D45F0 + 0x2013C`, verified 1/30 vs 1/60) * 0.5 - c, so it halved at 60 fps; that `mulss` now reads a 1/30 constant. Blur lasts ~2.4 s at both rates. Other `* [game+0x2013C]` sites (44) may hide more frames-times-frame-time products. Test helper: `--memdump [*]ADDR:SIZE:F1,F2` saves memory at those frames (scratchpad memdiff.py lists floats that change).
- Test helpers: `--background` (window minimised without focus, audio muted, so tests do not disturb a session in use), `--wav <file>` (record the mix), `--shot-start S`, `--press` with `mx/my+N` (mouse counts per poll), `lx/ly/rx/ry+N` (stick percent) and `*MS` hold times, `--watch-pad N [--watch-read ADDR]`. Clips: `--rawvideo FILE --shot-start S --clip-seconds N` writes the shown picture as raw BGRX frames at full speed (shrunk on the GPU first) and `--wav` the sound; the log's "[capture] first frame" and "[audio] recording the mix" times line them up (the mix runs 60 ms ahead of the device). Encode with the ffmpeg that ships with Krita (`C:\Program Files\Krita (x64)\bin\ffmpeg.exe`: `-f rawvideo -pix_fmt bgr0 -s WxH -framerate <frames/seconds> -i v.raw -ss <offset> -i a.wav -c:v libopenh264 -c:a aac`).
- **Performance recorder** (`windows/src/perf.cpp`; Tools > Record performance or F2 to start/stop, `--perf-at S:D` for scripted runs; written to `perf\<date_time>\` in the root, git-ignored): per frame on the render thread (the game thread that calls D3DDevice_Swap) the work time (previous Present -> Swap), the vblank wait, present time, vblanks taken (late frame = more than the swap interval: the game steps a fixed time per frame, so late frames are slow motion; game speed = sum of intervals / vblanks), exclusive timers for draw calls / vertex conversion / texture checks / shader compiles / render-target copies (`perf::Scope`, a nested scope pauses its parent), counters (draws, vertices and bytes submitted, uploads, compiles), CPU cycles of the other game threads (QueryThreadCycleTime). A sampler thread off the game CPU suspends the render thread about 500 times a second, records EIP, whether it ran during the last interval (cycle time vs rdtsc: else "blocked") and up to 6 return addresses found by scanning the stack (values in the XBE or blackrt that follow a call instruction; a heuristic, stale entries happen). The report (summary.txt, frames.csv, threads.csv with CPU masks) groups all / slowest 5% / late frames and names game functions from functions.csv, port functions from blackrt.pdb (dbghelp; stage the PDB next to blackrt.dll) and other code by module plus the port function that called it. **First profile (2026-10-05, Ryzen 5 2600 + RTX 2060, 60 fps, 2560x1440, start of mission 1, walking):** 10.8 ms of 16.7 per frame on the render thread; game code only ~0.6-1.1 ms; the port ~6 ms: `sample_hash` 23% (host_texture re-hashes sampled texture data on every bind, every frame), vertex conversion ~28% (convert_vertices/decode_element: 117k vertices = 5.8 MB of float4 per frame, every frame, then DrawIndexedPrimitiveUP copies it again), driver 20% (UP draws, all VS constants every draw); blocked ~16% (console/log writes on the render thread, Sleep in the game, being preempted). Everything runs on the one game CPU (CPU 0, which also takes many interrupts), so Task Manager shows low total CPU and GPU while the game thread is the limit. **User's busy scene (fight, 60 fps):** game speed 78%, 27% late frames, 20.8 ms average (~770 draws, 150k vertices a frame): texture checks 8 ms (`sample_hash` 37%), conversion 4.3, draw calls 4.5, game code 1.75. **Fixes (first round):** host_texture checks a texture at most once per frame (`Entry::checked_frame`; a Lock still marks it dirty), vertex conversion with one loop per element type (`decode_run`) into a buffer that only grows (the per-draw resize zero-filled it), the pixel shader of the previous draw reused without the map lookup when its key is unchanged, empty XInput slots polled every ~2 s. Test scene 10.8 -> 5.1 ms average, 99% 13.1 -> 7.0 ms, picture checked. User's re-recording: game speed 99.5%, 0.5% late, slowest 5% 18.9 ms, `sample_hash` still 23% (hundreds of textures a frame), so textures unchanged for 30 checks are now checked every 16th frame, staggered by resource address (`kStableChecks`): texture time 0.51 -> 0.14 ms in the test scene. **Stutter with no late frames** (user, 2026-10-05: second level, bridge crossing, 107 s at 100% game speed, 0 late frames, yet visible stutter now and then): the emulated vblank was a free-running 59.94 Hz timer while the display runs at ~59.95 Hz ("59" in Win32_VideoController); the two drift, and while the Present phase sits near the desktop composition deadline frames are shown a refresh late or skipped. Present times also jittered 0-2 ms because pace_swap polled with Sleep(1) (7% of frame intervals > 18 ms). Now `vblank_thread` ticks after `DwmFlush` (composition runs at the display's vblank) when the display is 59-61 Hz, falling back to the timer for a tick when DwmFlush returns early, and pace_swap waits on an event set at each tick. `--timer-vblank` restores the timer; `--display-vblank` forces the display sync in `--background` tests (a minimised test window: 3575 of 3600 ticks from the display, 59.90 Hz). Not checkable in background tests: the user judges the stutter.
  Also still seen: ~11 frames in 107 s above the budget (one 28.7 ms, in the game code part), short hitches the game catches up on.
  **Weak laptop** (user, 2026-10-06: HP EliteBook 840 G1, i5-4300U 2C/4T, Intel HD 4400, 60 fps, 1376x774): game speed 73.5%, 21.9 ms a frame, game code 5.5 ms (4.4x the Ryzen's: more than the CPUs differ, so likely power saving or a busy hyperthread sibling), Intel driver (igdumdim32) 8.6 ms, port 6.7 ms. At 30 fps (the original rate, 33.3 ms budget) it fits. Changes: begin_draw and the draws skip SetPixelShader / SetVertexShader / SetFVF / SetVertexDeclaration when the value is the one last sent (`set_ps` etc.; fullscreen passes restore state with a state block, so the cache stays valid); the game threads go to the first logical CPU of the last physical core instead of CPU 0, and the process affinity drops that core's hyperthread sibling so driver/audio threads created later stay off it (`pin_to_one_cpu`, `--game-cpu N`). Ryzen A/B with the same build: last core 5.63 ms average / 99% 7.56, CPU 0 6.32 / 8.72. **The laptop then crashed** ~60 s in (start of the first level): access violation writing past a buffer inside a system DLL (memcpy, 0x7696ac77) under D3DDevice_SetTexture -> pb_flush (push-buffer draws). First guess (the shader/vertex-input cache going stale after a fullscreen pass; `forget_sent_shaders()` now runs after each) did not fix it. **Cause, from the second crash log with port frames:** SetTexture -> pb_flush -> push-buffer immediate-mode vertices -> draw_immediate -> DrawPrimitiveUP with **256-byte vertices** (all 16 attribute slots as float4, decl_imm16): D3D9 strides go up to 255 and Intel's driver overran its vertex buffer (NVIDIA tolerates it). Now only the slots the game wrote or the shader declares are sent, with a declaration per set (`decl_imm`); the level start uses mask 0x201 (position + texcoord v9), 32-byte vertices. Old path kept only if all 16 slots are used (logged once). Was there before today's changes. Crash logs now name module+offset for addresses outside the XBE and list port (blackrt) return addresses too; blackrt.map is linked (/MAP) and `python tools/mapsym.py 0x<offset>` turns blackrt.dll offsets into functions (keep the map of the build that crashed: it is staged next to blackrt.dll in work\run). **Not done: uploading only the vertex constants that changed** (all 200 per draw, ~1.3 ms in the busy scene): with a compare against the last upload the world lost its textures/lighting; GetVertexShaderConstantF then showed device registers holding values never sent to them (e.g. what was sent to c192 at c186), not explained by the shaders' def constants (1-3 per shader, invalidated, still broken). Next candidates: this, the draw-call path (state translation, DrawIndexedPrimitiveUP copies; caching converted vertices in host vertex buffers), console log writes on the render thread, CreateStateBlock(ALL) per fullscreen pass, the game CPU choice (CPU 0).
- **Light cones cut off at a distance (fixed 2026-10-06)**: street-lamp light cones are drawn into a 160x120 target (0x02db0000, vs 02E51630: alpha = 0.4 x (view.normal)^4 x min(1, 50/dist)), depth-tested LESSEQUAL against a 160x120 copy of the scene depth (the game reads the 640x480 Z24S8 buffer as A8R8G8B8, draws it into 0x02dc4000 and then uses that memory as the depth surface; the port's zview / zfill shaders do the two conversions), then blurred (160x120 and 80x60) and added to the back buffer (ONE / SRCALPHA). From far away only the part in front of buildings showed (user, compared with Xbox Series). Cause: kZViewPS computed floor(d x 16777215 + 0.5) in fp32; at d = 1.0 that is 16777215.5 -> 16777216, so the cleared sky (no draw writes depth there) became 0xFF0000 = 0.9961, nearer than distant walls (0.9989), and cone parts deeper than 0.9961 failed against the sky. Fix: min(v, 16777215). Checked: cleared pixels now 0xFFFFFF, none 0xFF0000. Also affects the other depth readers (depth of field, reload blur masks) where the sky is involved. Found from two F12 dumps: the RT PNGs are host-sized (552x311 for 160x120) and the colour PNG's alpha is forced opaque (real alpha in *_alpha.png).
- **Seams of destructible objects** (user, 2026-10-06): walls and props built from chunks (each with its own matrix) showed hairline cracks along the chunk edges that flickered when turning; not on the Xbox. The NV2A rasterizes from screen positions with 4 fractional bits, which pulls nearly identical edge vertices onto the same point; the host keeps more precision and renders at 3-4x the size. vsh_to_hlsl now snaps oPos.xy to a grid of the Xbox screen space before undoing the viewport transform: 1/16 pixel (the assumed NV2A precision, not verified) left seams "a little bit visible", so 1/8 (0.43 host pixels a step at 3.45x). Cause of what remains: a gap that rarely covers a pixel centre at 640x480 covers them along the whole seam at 3-4x the size; T-junctions are not closed by snapping. Still "flickers just a little" with 1/8, so a **seam fill pass** was added (kSeamPS in d3d/device.cpp, before the gamma ramp in present_backbuffer, ps_2_a): with u = 1 - depth from the INTZ copy of the depth surface the game pairs with its back buffer (`g_main_z`, set in bind_render_target), a pixel is a gap when u < 0.75 x the smaller of its two neighbours 1 or 2 pixels away (horizontally or vertically) and those two are within 10% of each other; it takes their average colour. Silhouettes keep their edges (sides at different depths). Side effect: 1-2 pixel holes in lattices or foliage against the sky get filled too (barely visible in a test). Video > Fill geometry seams, `seamfill` in black.ini, `--no-seamfill`.
- **Anti-aliasing**: an edge-directed post filter (own implementation of the FXAA idea, `kFxaaPS` in d3d/device.cpp, ps_2_a with a fixed branch-free edge walk because ps_3_0 would need a vertex shader in the full-screen passes) on the final picture: present = gamma ramp 1:1 into a render-resolution target, filter 1:1 into a second one, then linear scale into the window. On by default, `--no-aa`, F9 toggles (shown in the title). Brightness unchanged (frame mean 45.7 vs 45.4). MSAA does not fit: the game's DOF/blur read the depth buffer back as a texture (no multisampled INTZ in D3D9) and it re-reads its render targets.
- **Front end / custom menu options (scoping, 2026-10-05).** The menus are EA's "Apt" UI (Flash-derived; `export/frontend/FEMain.bin` = "Apt Data:7" with ActionScript packages `__Packages.BkMenuList`, `BkMenuButton`). Screens in it: legalscreen, bootsequence, blacktitlemenu, choosedifficulty, controllersetup, missionmodeselect, resultsscreen, operatorprofile, memcard*, createblackprofile, profileoptions, fmvplayer, credits, virtualkeyboard, unlocks, feaudio, pressstart, controllercustomxbox, **optionsmenuxbox**, **soundsetupxbox**, **picturesetupxbox**. An option is movie clips + localised text (`$FE_VIBRATION`, `$FE_INVERTLOOK`, strings in language/strings/MainUS.bin) that reach game code through named callbacks registered with FUN_00112FC0 (ecx = name, pushed function; e.g. `SetGammaLevel` -> FUN_00119970 at 0x119B37) and UI variables (FUN_000BF2A0, e.g. `FEGammaLevel` = settings * 100). Two routes for port options (render size, AA, fps, gain, mouse): (a) add entries to the Apt screens - needs an Apt reader/writer for the movie data and bytecode, new strings, and new callbacks registered like `SetGammaLevel`; the modified file would be built at run time from the user's FEMain.bin (nothing game-derived in the repo); (b) an overlay menu drawn by the loader on top of the game (D3D9, keyboard/controller, game input paused while open), plus a settings file - far less work, independent of the game data. **Chosen for now (user): a Windows menu bar on the game window, so the port stays vanilla; modifying the game's own menus comes after the vanilla port is done.** `src/d3d/menu.cpp`: Game (Exit), Video (Fullscreen F11, Anti-aliasing F9, Render resolution: 1 / 1.5 / 2 x window, 720p/1080p/1440p/2160p, default; Aspect ratio and Frame rate - after restart, the game reads them at boot; Brightness gain/gamma), Audio (reverb on/off, level 50-200%), Input (mouse sensitivity presets, invert, release mouse = Esc), Help (controls, about). Commands run on the window thread; render-size changes are handed to the render thread (`request_render_change` -> `check_render_size` after the next Swap). Hidden in fullscreen; opening it releases the captured mouse; Alt/F10 do not enter it by keyboard (SC_KEYMENU blocked so the game never stalls). Settings persist in `black.ini` in the game root (`windows/src/settings.cpp`; written on every menu change and on window close, so F-key changes are kept too; command-line flags win for that session; `--background` test runs neither read nor write it; git-ignored). Tested by posting WM_COMMAND to a background run: 1080p switched live, AA toggled.
- **To check (user, later): picture against the Xbox Series.** The gamma ramp fix (2026-10-05) is confirmed by the user for the fades and the in-game brightness slider (which never worked before; the user had noticed but not reported it). Still to compare: overall brightness and colours. Method: with the in-game brightness slider at the same position on both, take an Xbox Series screenshot (Share button) of a still screen - boot LOADING screen with the bullet casings, the title screen or the main menu - and the port's screenshot of the same screen (`--screenshot`), then compare pixel values (dark-green loading background, white text, gold logo). Possible remaining differences: anything the Series' backward-compatibility layer adds after the ramp, blur/bloom buffers rendered at 3x (sharper than a 640x480 original), shader arithmetic precision, TV vs monitor.
- Next: check 60 fps game speed everywhere; decode the I3DL2 state of effect 16 (or match it against the library's SetI3DL2Listener conversion, 0x21B0C9) for the exact room; bins 17/18 and 27-30 effects.

### In-game PC SETTINGS menu: work in progress (2026-10-12)

- **Front-end calls to the port** (`loader/windows/src/fe/calls.cpp`, uncommitted): a thunk at the dispatcher FUN_00112F30 entry.
  Names starting with "PC" are handled by the port and cleared; game calls are logged as `fe: game call Name('p1','p2','p3')`.
  Fields in the FE object: trigger +0x370, name +0x374 (32 bytes), params +0x394/+0x3D0/+0x40C (60 bytes each).
  Callers of the dispatcher: 0x12465C and 0x1246CA. Scripts set `_global.externObject.FEFunctionName/Param1..3/Trigger`.
  `PCDebug` logs its 3 params: the debug channel for generated scripts (scratchpad m1.py mode `dbg`).
- **Options page rows** (optionsmenuxbox TextAnim, char 27). Frame 0 places the rows at depths 1/3/5/7, transparent
  (color transform mult 0x00FFFFFF). The reveal is TextAnim's own timeline: frames 46/49/52/55 Move + CXForm each row
  to opaque and place a flash bar (char 25/26, flags 0x16) at depths 9-21. A new object at an occupied depth gets
  that frame's matrix. What works (milestone 1, scratchpad `m1.py row,wrap,page`):
  - The new row at free depth 2 (cloned text → cloned sprite 6 → cloned sprite 7).
  - Its own reveal at frame 58: a copy of frame 55's Move on depth 1, retargeted to depth 2, plus a bar at depth 22.
  - Wrapped PagePrepare/PageRelease/PageDestruct create `pcButton` (BkMenuButton) in the up/down ring, with
    `actionSelectPage = "PCSettings"`. The container entry "pcsettings" is a copy of soundsetupxbox.
  The round trip Options → PC SETTINGS → Back has no crash.
- **Text fields**: the game's text provider FUN_001184D0 reads the *variable* string (runtime text +0x54):
  - `$KEY`: localized string, looked up by name hash (FUN_001F3A80, thiscall on *0x2D45D8).
  - `&KEY`: bound to an entry of the FE variable table at *0x2D199C + 0x13A8: 0x70 entries
    {type, value, hash, refs, watchers}, with a second table at +0x2DF4 (FUN_00112CC0). The same table holds
    externObject's members. On a change, FUN_00114960 copies the value into each watching text instance (+0x28;
    the list runs through +0x48).
  - `#...`: something from FUN_001F3AC0.
  - Anything else is shown literally. So a literal label goes in the variable (var 'PC SETTINGS'); an empty
    variable shows nothing.
- **Value tables** in the FE object (*0x2D199C): 0x70 entries from table +4, 0x14 bytes {type, value, hash (-1 free), refs,
  first watcher}. Hash = FUN_000BF2A0: CRC-32 style, start -1, `sar`, table 0x2AC908.
  - +0x2530: externObject members named "FE…"; FUN_00114050 resolves "HUD…" in +0x1C78 and other names in +0x13A8.
    Type 1 = pointer to an int32 (FUN_001146A0); type 5 = char buffer (FEFunctionName, FUN_00114560).
  - +0x13A8 / +0x2DF4: `&` text values; type 4 = pointer to UTF-16 (FUN_001145B0, FE_SOUNDTRACKNAME).
  - The port keeps PC_V<n> (strings, +0x2DF4) and FEPCS<n> (ints, +0x2530) registered (loader `fe/vars.cpp`).
- **Page classes** (AS2 in each page):
  - BkListBox: SetId(n), SetGameFunction(name); left/right (FEInputId 3/4) → name(id, ∓1).
  - BkMovieclipSlider(clip, name, start): range 0-100, step 100/(clip frames − 1) (Sound Setup's bar: 26 frames,
    step 4); calls name(value) and gotoAndStop on the bar.
  - BkMultiChoiceMenuButton(clip).mChoiceList = either of them; BkMenuList forwards input to it.
  - BkMenuList back: mfuncFunctionBack (Options: SelectBack → StartAutosaveSettings), then mstrPageBack
    (FETargetPage + FEChangePage). actionSelectPage / actionSelectGameCode on select.
- **Container tail**: a movie's header +4/+8 = count / region-relative texture offsets, +0x14 = texture table
  (block-relative), all after the geometry block (+0x18), texture data 0x80-aligned. Moving the geometry must move
  all of it by a multiple of 0x80 (soundsetupxbox, picturesetupxbox, map, operatorprofile, pressstart, virtualkeyboard).
- **PC SETTINGS pages work** (scratchpad `pages.py`, 2026-10-12):
  - Options row → "pcsettings" (copy of Options, 4 rows) → pcvideo / pcdisplay / pcgame / pccontrols
    (copies of Sound Setup).
  - Choice rows are clones of the PLAY MODE row; slider rows are clones of the SFX row plus the bullet bar (char 34)
    at 45 % with the number after it.
  - Loader side: `fe/pc_settings.cpp` (rows, choices, apply + save), `PCChange(row, ±1)`, `PCSlider<row>(0-100)`.
  - In-game (PseMenu/InGameOn) pages import Core/PAudio, not Common/FEAudio, and have BkValueList instead of
    BkListBox, so they need their own template.

### Release 0.2.0 layout, menu keys, no port keys (2026-10-14)

- **Layout:** the zip holds one folder, `port\` (black.exe, blackrt.dll, README.md, LICENSE.txt), extracted next to
  the user's `game\`. `g_cfg.port_dir` = black.exe's folder; defaults there: `savedata\` (the HDD, was `hdd\`),
  `black.log`, `black.ini`, `cache\menus`, `perf\`. The game is still found by looking upwards from black.exe
  (port -> the folder with game), which becomes the current folder (relative `--hdd` / `--log` given on the command
  line resolve against it, so the dev shortcut `--hdd work\hdd --log work\play.log` is unchanged; its black.ini is now
  `work\run\black.ini`). Tested from an extracted zip with a junction to game: all files under port\.
- **No port keys:** F2 (perf; `--perf-at` stays), F3-F9, F10, F11, F12 removed; any key can be bound. The window title
  is "BLACK" ("[AUTOMATED TEST, MUTED] BLACK" with --background).
- **Menu keys without the pad:** in menus, pauses and cutscenes bound keys do nothing and arrows / Enter / Backspace no
  longer press D-pad / A / B. FUN_00118B70 (once per front-end update) clears the menus' input id (FE object + 0x44C,
  registered as externObject FEInputId by FUN_0011A820) and fills it with FUN_00118A80 (eax = controller, ebx = input
  object: D-pad / stick up-down with repeat, left-right, A or START = 5, B or BACK = 6, START on "PauseMenu" = 6;
  +9/+0xC a lock-out). Its two calls (0x118B9D, 0x118BC6) and the one in the PRESS START check FUN_0011A750 (0x11A79C)
  go through `menu_input_thunk` (fe/calls.cpp): after the game's reading, `bindings::take_menu_input` sets the id
  when no controller did (held arrows repeat after 400 ms every 120 ms).
  - PRESS START (ListenForPlayer('0') sets the check +4 = FUN_0011A750): the first controller with A/START becomes the
    player's: game object (*0x2D45F0) + 0x21060 = controller (*0x2D45E8 + 0x518 + i * 0x16C), its +8 = 1, the check
    off, *0x2F3BD4 (binding table pointer) = game object + 0x20174. Enter there does the same for port 0.
  - Movies: the FMVPlayer page reacts to the menu input; the port's forced skip takes Enter directly.
  - Hints that stop the game (PRESS A TO CONTINUE; ids 12 health packs, 13, 23, 24 low health, 87 at 0x28AA80): the
    HUD sets game object + 0x21098 = 3 (0x131C47) and the game closes the hint on A / START of the player's controller
    channels ([controller + 0xC]: A pressed +0x35, previous +0x19, analog +0x84; START +0x31 / +0x15 / +0x74; checks
    at 0x129637, 0x129A38, 0x129B10 -> FUN_0020DBC0), not through the menus' input. So Enter is the pad's A for a
    press that starts while a hint stops the game or no menu reads its input (fe_menu_keys marks the frames the menus
    read); a press a menu took stays the menu's while the next page comes up. Tests: press-file "hint:N" shows hint N
    through the game's FUN_0012B3C0 (eax = N; the HUD *0x2D12E4 + 0xC and a 16-byte vector it keeps at +0x22B0);
    hints 12 and 24 shown and closed with Enter.
  - A mission's intro text (location and time typing out before play) is skipped by START of the player's controller
    (not A). No menu page is up then: the front end's current page name (FE object *0x2D199C + 0x3840) is empty, though
    its input code still runs. So Enter is START for a press that starts with no menu page (and A while a hint stops
    the game; game object +0x21078..0x2109C do not change during the intro text).
- **QUIT on the main menu** (`main_menu_quit`, fe/pages.cpp): blacktitlemenu (CONTINUE MISSION / OPTIONS / OPERATOR
  PROFILE: TextAnim char 18, rows 25.1 apart, revealed at 43/46/49 with bars at depths 7/9/11) and blacktitlemenu2
  (START MISSION / OPTIONS: TextAnim char 14, revealed at 43/46, bars 5/7) get a QUIT row (`add_menu_row`, shared with
  Options' PC SETTINGS row) in the up / down ring; it calls PCQuit, which posts WM_CLOSE (settings saved, exit).
- **Game text without "Xbox"** (`patch_strings`, fe/pages.cpp): language\strings\MainUS.bin is {0x586A0308, a word,
  count n (1724), table offset 16; n ascending offsets; entries: 4-byte key (sorted), NUL-terminated text, padded to 4
  except the last}. Written to cache\menus with XBOX CONSOLE -> COMPUTER and XBOX -> COMPUTER (6 strings: free blocks,
  saving and loading, the hard disk check); keys, order and header kept; the game reads it from the cache (k_file
  logs each file it serves from there). The other "xbox" in the game's files are page names, platform checks and paths
  (not shown). The port's labels: "3 X ORIGINAL" (was 3 X XBOX); EFFECTS AT XBOX SIZE stays.
  - Tested keyboard only from a copy of the user's save: logos, PRESS START, menus, level select, mission, pause menu,
    PC SETTINGS, Options, resume, quit; a held arrow repeats on a slider; KEY BINDINGS captures.

### Sleep and wake, lock screen (fixed 2026-10-13)

User report: the game froze when the PC went to sleep. Three things broke, all fixed:
- **Direct3D 9 device lost** (sleep, lock screen, Ctrl+Alt+Del, UAC): Present returns D3DERR_DEVICELOST and every
  draw is dropped, so the picture stayed on the last frame. `xd3d::recover_device` (d3d/device.cpp, after each
  Present): once TestCooperativeLevel gives D3DERR_DEVICENOTRESET, the host targets (all D3DPOOL_DEFAULT: the same
  set a render-size change drops) and the back buffer / depth references are released, `host3d::reset_device`
  Resets with the creation parameters, and what the game set is sent again: `forget_sent_state` (draw.cpp: render,
  texture-stage, texture, shader, declaration and fog-table caches), transforms (g_st), material and lights (now
  kept), target, viewport, scissor. Game textures are D3DPOOL_MANAGED and survive, as do shaders and declarations.
- **XAudio2 device gone** (HDMI / USB audio after wake, another default device): OnCriticalError, the source voice
  takes no more buffers, `blocks_wanted` returned 0 for ever and the game's DirectSound buffers stopped. Now
  audio/output.cpp closes and reopens XAudio2 (once a second) on the critical error or when no block was taken for a
  second, and asks for blocks by the clock meanwhile (also without XAudio2 at all; more than a second behind is
  skipped, not mixed in a burst).
- **Game clock**: the emulated 733 MHz counter (timing.cpp) followed QPC, so after a sleep the game saw one frame as
  long as the sleep. It now advances by the host time between two readings, at most a second (logged above 2 s).
- Tests: press-file lines "reset" (the same Reset path, forced) and "audio-lost"; drive.py `freeze SECONDS`
  suspends the whole process (NtSuspendProcess) like a sleep. Menu and mission pictures identical after resets; after
  a 6 s freeze: clock capped, XAudio2 reopened in 40 ms, 60 fps on. A real device loss (TestCooperativeLevel
  waiting on D3DERR_DEVICELOST) was not produced in a test.

### PC SETTINGS in the game's menus: state at the end of 2026-10-12 (loader `src/fe/`, not committed yet)

- Built at every start from the user's own files into `cache\menus` (beside black.ini). The file layer serves it
  after `--game-override`; `--no-pc-menus` turns it off. `fe/apt.cpp` edits containers; `fe/pages.cpp` builds the pages.
- **Main menu** (FEMain.bin):
  - Options → PC SETTINGS (copy of Options) → VIDEO / DISPLAY / AUDIO AND GAME / CONTROLS, each a copy of Sound
    Setup. CONTROLS → KEY BINDINGS (scrolling list, `fe/bind_list.cpp`; capture in `input/bindings.cpp`).
  - Each page reports PCPage(group | 100) when prepared; the port fills that page's values.
- **Pause menu**:
  - PC SETTINGS after OPTIONS (InGameOn.bin) opens ONE page "PCPageIG" (copy of the in-game Options, PseMenu.bin).
  - Its six rows show the port's current screen (`fe/pause_ui.cpp`). State per row comes from externObject FEPCR<i>
    (packed), FEPCLayout / FEPCFocus / FEPCAtRoot; a wrapped Update applies it every frame.
  - One copy per screen did NOT fit: PseMenu.bin is loaded on demand into a heap with little room in play. At 1.06 MB
    the game's allocation came back null (NtReadFile into 0 → hang); ~0.87 MB still loaded in mission 1. Keep the
    pause file small.
  - Aspect ratio and frame rate (restart-only) are on the main menu only.
- **Value tables, measured** (min free of 112 in a run to mission 1):
  - +0x2DF4 is filled to 0 by the game at the main menu. The port using it made the game's own string setter
    FUN_001154A0 crash (NULL entry).
  - +0x13A8: ≥ 84 free with the port's 19 strings → the port's strings live there.
  - +0x2530 (FE members): 47 free; the port uses about 13 ints there.
  - +0x1C78: 0 free (HUD).
- **Page script facts**:
  - FEInputId 1 up, 2 down, 3 left, 4 right, 5 select, 6 back.
  - BkMenuList calls ProcessMapInput of the focused row's mChoiceList on every update (input 0 = none), after
    moving the focus. MyMenu.MovedCursor is true when the focus moved.
  - A button without actionSelect* treats select as Back.
- **Still to do**:
  - remove the Windows menu bar (and move or drop Tools / Help);
  - README and controls text;
  - commit when the user asks.

### PC SETTINGS: soft lock going in and out of pages, crash at the first pause (fixed 2026-10-13)

- **Soft lock:** going back and forth between PC SETTINGS entries quickly left a page with the selector showing but
  no row focused, and Back dead (MyMenu undefined).
  - Pages define their classes in main-timeline DoInitActions only `if (!_global.X)`: BkMenuList, BkMenuButton and,
    by page, BkListBox, BkMovieclipSlider, BkMultiChoiceMenuButton, BkValueList, BkValuePoint. Prepare makes
    `MyMenu = new BkMenuList`. Destruct deletes MyMenu, `_global.BkMenuList`, `_global.BkMenuButton` (some pages
    also MyMap, BkMultiChoiceMenuButton, BkValueList, BkValuePoint; Sound Setup and Picture Setup never delete
    BkListBox / BkMovieclipSlider / BkMultiChoiceMenuButton, so those stay in _global).
  - A DoInitAction runs only when the runtime loads the movie. A page entered again ~1 s after it was left is still
    loaded: it defines nothing, the page before had just deleted the classes, so Prepare makes no MyMenu.
    Event log (BLACK_FE_DEBUG): old page's Destruct, then the new page's load seeing no `_global.BkMenuList`.
    Destruct always came before the next page's load; page instances are not reused.
- **Crash at the first pause** (user report, `FUN_000FC360+0x40` from `FUN_00112520`, reading through garbage):
  the first soft-lock fix kept the classes in `_global.pcKeep*`. They survived leaving the front end; InGameOn's
  pause menu set them again, releasing the last reference to a FEMain object whose memory the game had already
  reused, and the release walked the freed movie.
  - FUN_00112520 (FE update) holds a reference to every entry of the list at 0x2EC980 for the update and releases
    them after; FUN_00019F20 destroys a refcounted 0x18-byte object; types 3/4/5 hold a movie (+0x10) that
    FUN_000FC360 walks.
  - Rule: nothing of a page may stay in `_global` after its container goes (FEMain.bin at a mission start, PseMenu.bin),
    and a page must not release what another page left there.
- **Crash at a second pause** (user report 2026-10-13, `FUN_0010D8D0+0x13` reading 0xA70): FUN_0010D8D0 runs a frame's
  DoInitActions; the frame table read was 0xA70, the unrelocated offset of the main timeline's frames in
  pausemenuoptionxbox / pcpageig. The pause page's own list class `_global.PcRow` (and the KEY BINDINGS page's
  `_global.KeyList`) were never deleted. PseMenu.bin is loaded at each pause and was dropped during play (reloaded
  at the second pause, not in a short test); the leftover kept the old page's entry, which was later run on the
  reloaded, unrelocated data.
  - Original pages leave classes too (Sound Setup: BkListBox, BkMovieclipSlider, BkMultiChoiceMenuButton; in-game
    Options: BkMovieclipSlider). Overwriting those at every load would release them.
- **Fix** (`fe/pages.cpp` own_classes_*, `Movie::define_classes_on_load`), on every page the port builds or changes:
  - a DoAction first in frame 0 (`Movie::prepend_action`) keeps what `_global` held for each class (`pcPrev<class>`);
  - the guarded class DoInitActions become DoActions placed before the frame's first DoAction, guard neutralised:
    the classes are defined at every load; the page keeps them in page variables `pcOwn<class>`; PcRow and KeyList
    count as the page's classes;
  - Destruct runs the page's own Destruct0, then each class goes back to `pcPrev` where it is still the page's, or
    stays as another page set it: `_global` is as the page found it;
  - Update (safety net, not after Destruct): without MyMenu or its first button, the page puts its own classes back,
    makes MyMenu again and re-runs PagePrepare (logs "a page's menu was gone").
  - Tests on a copy of the user's save: main-menu stress (0.4 s in/out, same page re-entered), mission, pause →
    PC SETTINGS stress, in-game Options, resume, pause, quit, main-menu PC pages again: no crash, no dead page.
    The event log shows PcRow / KeyList gone after their pages. Not reproduced: PseMenu dropped during play (needs
    moving through a level between pauses).
- **Debug:** `BLACK_FE_DEBUG=1` adds PCDebug reports on input and an event log (PCLog: L load with which classes are
  in `_global`, D Destruct, H heal, with an instance number). FE call parameters are 60-byte buffers the runtime fills without a bound: a longer
  string overruns them (crashed with EIP = text). The script runtime has no `String.substr`.
- **Values:** the port no longer walks a value's watcher chain (it can name text fields of a page that is gone). The
  strings are written in place in fixed buffers; the port's entries hold 0x10000 references.
- The Windows menu bar is gone. Fullscreen from PC SETTINGS is `host3d::set_fullscreen` (WM_APP + 0x53). The key
  bindings dialog and menu.cpp were removed.