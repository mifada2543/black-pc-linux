# Cara memasang BLACK di Linux (port i386 native)

Dokumen ini cara memasang dan menjalankan game di port Linux. Semuanya di sini sudah terverifikasi di
mesin ini kecuali bagian yang ditandai **belum terverifikasi** — yaitu bagian yang memang butuh file
`default.xbe` asli.

> Ganti `blacklinux` dengan `loader/build/linux/blacklinux` kalau kamu memanggil dari root repo.

---

## 0. Status jujur sekarang

| Bisa | Belum |
|---|---|
| Loader native 32-bit boot image XBE: parse, map, patch `fs:` → `gs:`, resolve 115 import | **Belum ada D3D8/game loop** — device Vulkan sudah ada, tapi belum ada yang menggambar game |
| HLE kernel: memori, file, sync/IRQL, Rtl, thread, AV/HAL, crypto | **DPC/timer thread belum** (`KeSetTimer` & 5 ordinal lain masih stub) |
| Device Vulkan: target, depth, texture, sampler, pipeline, clear, readback, PNG | **Gambar belum ada**: driver 32-bit di mesin ini tidak merasterisasi apa pun (lihat §8) |
| SDL2 host, audio, input | Audio & input belum |
| 15 suite test, 854 check, hijau | Verifikasi terhadap image asli (menunggu `default.xbe`) |

Artinya: begitu `default.xbe` ada, loader akan menjalankan kode game sedekat mungkin sampai panggilan
grafis pertama — dan di situlah belum ada apa pun. Klaim yang tidak bisa dibuat hari ini: game-nya
sampai mana, dan apakah boot-nya benar. Yang bisa diverifikasi tanpa file asli sudah diverifikasi
(15 suite, 854 check).

---

## 1. Paket sistem

Pasang sendiri (loader tidak pernah memakai `sudo`):

```sh
# Wajib: toolchain 32-bit
sudo apt install build-essential gcc-multilib g++-multilib cmake ninja-build

# Wajib untuk menjalankan: pustaka 32-bit (libc6-dev-i386 sudah ikut gcc-multilib)
sudo apt install libc6-dev-i386

# Untuk graphics: Vulkan 32-bit + driver Mesa 32-bit + kompilator GLSL ke SPIR-V. Lapisan device sudah
# ada (src/gfx/), jadi tiga paket ini bukan lagi opsional. glslang-tools dipakai saat build untuk
# mengubah shader jadi SPIR-V yang ditanam di dalam loader.
sudo apt install libvulkan1 libvulkan-dev:i386 mesa-vulkan-drivers:i386 glslang-tools vulkan-tools
```

Cek cepat bahwa semuanya ada:

```sh
file loader/build/linux/blacklinux     # harus: ELF 32-bit LSB ... Intel 80386
./loader/build/linux/test_vulkan       # harus: "Vulkan is usable from a 32-bit process"
```

Kalau `test_vulkan` tidak ada, `CMakeLists.txt` melewati build-nya karena `find_package(Vulkan)` gagal —
itu warning saat configure, bukan error.

---

## 2. Build

```sh
cd loader
cmake --preset linux
cmake --build --preset linux
ctest --preset linux          # 15 suite, 854 check
```

Hasilnya `build/linux/blacklinux` (ELF 32-bit, PIE, link ke `/lib/i386-linux-gnu`).

---

## 3. Meletakkan file game

Loader mencari `game/default.xbe` secara default (relatif terhadap direktori kerja). Yang penting:

> **DVD = folder tempat file XBE berada**, dan folder itu.read-only. Jadi seluruh isi disc Xbox
> (XBE + data + video) harus ada di folder yang sama. Loader tidak mencari disc lain.

```sh
mkdir -p game
cp /path/anda/default.xbe game/default.xbe
cp -r /path/anda/data game/          # kalau punyanya ada file data terpisah
```

Kalau tidak mau menaruh di `game/`, cukup `--xbe /path/ke/default.xbe`; folder DVD otomatis mengikuti
file itu.

---

## 4. Layout folder yang dipakai loader

Diberi tahu dari kode (`xf_init`), bukan dari tebakan:

```
<folder XBE>/                 -> \Device\CdRom0        (read-only)
$XDG_DATA_HOME/blacklinux/    -> \Device\Harddisk0\Partition0
  Partition1/ .. Partition5/  -> \Device\Harddisk0\Partition1..5
    Partition1/TDATA/         -> data judul (dibuat otomatis)
    Partition1/UDATA/         -> save game (dibuat otomatis)
```

`$XDG_DATA_HOME` biasanya `~/.local/share`. Override dengan `--hdd <dir>` kalau mau menyimpan save di
tempat lain (misalnya flash disk).

---

## 5. Menjalankan

```sh
# Melihat header/section/import tanpa menjalankan
./loader/build/linux/blacklinux --info --xbe game/default.xbe

# Menjalankan: boot, masuk entry point, tunggu --timeout detik, lalu keluar
./loader/build/linux/blacklinux --probe --timeout 30 --xbe game/default.xbe

# Menjalankan dengan jejak lengkap di console
./loader/build/linux/blacklinux --probe --timeout 30 --trace --xbe game/default.xbe
```

| Opsi | Arti |
|---|---|
| `--xbe <path>` | file XBE (default `game/default.xbe`) |
| `--info` | cetak header/section/import lalu keluar, tanpa map |
| `--probe --timeout N` | masuk entry point, tunggu N detik, keluar |
| `--trace` | semua baris detail + setiap panggilan stub ke console |
| `--strict` | panggilan kernel yang belum diimplementasi menghentikan run (keluar dengan kode 2), bukan cuma log |
| `--log <path>` | file log (default `blacklinux.log`) |
| `--hdd <path>` | folder writable untuk `\Device\Harddisk0` |

Semua argumen berawalan `--`; loader tidak menerima argumen posisional, dan `--xbe <path>` selalu
ditulis eksplisit kecuali memang memakai default `game/default.xbe`.

Tanpa `--probe`, program menunggu entry point selesai sendiri — dan game aslinya tidak pernah balik,
jadi `--probe` + `--timeout` adalah cara menjalankan yang benar untuk sekarang.

---

## 6. Membaca log

Baris yang perlu diperhatikan dari `blacklinux.log`:

```
teb: %gs:0 = 0x... &xteb = 0x... K = 0xfffffde8      # jangkar per-thread, K dihitung saat runtime
file: \Device\CdRom0 -> ... (read-only), \Device\Harddisk0 -> ...
memory: contiguous RAM at 0x80000000, 64 MB
xbe: 'BLACK' retail build, base 0x00010000, entry 0x00101000, 3 sections, 115 kernel imports
fs: 6 instructions rewritten to gs, 0 unrecognised
kernel: 115 imports resolved (96 implemented, 13 data exports, 6 logging stubs)
thread 'boot' created: routine 0x00101000 ... handle 0x40000000 id 1
```

`[k] UNIMPLEMENTED <Nama>(argumen) ordinal N, call #1` = stub. Ini **baris paling berguna di seluruh
proyek**: begitu XBE asli ada, daftar ini langsung memberi tahu ordinal mana yang dipanggil game
duluan, jadi itu yang dikerjakan berikutnya. 6 yang tersisa semuanya DPC/timer
(`KeSetTimer`, `KeInitializeTimerEx`, `KeCancelTimer`, `KeInitializeDpc`, `KeInsertQueueDpc`,
`KeRemoveQueueDpc`).

Kode keluar: `0` normal (termasuk `HalReturnToFirmware(0)` = halt), `1` error fatal yang dilaporkan game
atau gagal start, `2` argumen salah **atau** `--strict` menghentikan run, `4` bugcheck (`KeBugCheck`).
Kode ini diuji oleh `tests/test_exit.cpp`, jadi bukan hasil kebetulan.

---

## 7. Kalau tidak jalan

| Gejala | Arti |
|---|---|
| `an unknown ordinal is refused by name: 9999` | Nama ordinal di XBE tidak ada di tabel 115 — update `ktable_posix.cpp` |
| `teb: K mismatch` | Offset TLS thread ini beda dari yang dipatch; patcher harus menghitung ulang (seharusnya tidak terjadi) |
| `fs patch: unhandled fs:[0x...]` | Displacement itu di luar blok 0x2000 — blok perlu diperbesar, atau field KPCR itu belum ada |
| `boot: could not create the boot thread` | Gagal `pthread_create` (stack/limit) |
| `boot: the game ended the run itself: ...` | Game memanggil `HalReturnToFirmware` / `HalInitiateShutdown` / `KeBugCheck` — itu bukan crash loader |
| Crash di panggilan graphics | Diharapkan: graphics belum di-port |

---

## 8. Grafis: yang sudah jalan dan yang tidak

Lapisan device Vulkan sudah ada (`src/gfx/`) dan teruji headless, tanpa window dan tanpa XBE:

```sh
./loader/build/linux/test_gfx          # 26 check, 2 di-skip (lihat bawah)
```

Yang terbukti di sini: device & queue, target warna dan depth, texture, sampler dari deskripsi D3DTSS,
pipeline dari SPIR-V yang dikompilasi saat build, clear (penuh dan persegi), readback byte-exact, dan PNG
valid. Yang **belum** terbukti: gambarnya, karena **proses 32-bit di mesin ini tidak bisa merasterisasi
apa pun**:

| Driver 32-bit | Hasil (diukur dengan program dari nol) |
|---|---|
| Intel (ANV) | segfault di `vkQueueSubmit`, untuk command buffer apa pun |
| llvmpipe (Mesa) | submit sukses, lalu **0 fragmen** dari 3072 piksel; `vkAllocateDescriptorSets` segfault |
| loader 32-bit | tidak mengekspos push descriptors |

Karena itu loader **mem-probe tiap GPU di child process** dan melewati yang mati (bisa dilihat di log:
`gfx: Vulkan device '...' skipped -- its driver died on submit (signal 11)`), dan `test_gfx` **mengukur
dulu** apakah ada fragmen sebelum mengklaim apa-apa — kalau tidak ada, check-nya di-`skip` dengan alasannya,
bukan dilaporkan lulus. Suite itu jujur, bukan apa pun yang tidak diukur.

Bisa memaksa device:

```sh
BLACKLINUX_VK_DEVICE=llvmpipe ./loader/build/linux/blacklinux ...   # pilih berdasarkan nama
BLACKLINUX_VK_PROBE=0  ./loader/build/linux/blacklinux ...          # lewati probe (untuk menelusuri probe)
```

## 9. Tanpa XBE: fixture

Supaya loader bisa dijalankan dan diperiksa tanpa file asli:

```sh
./loader/build/linux/fixture_dump /tmp/opencode/boot.xbe
./loader/build/linux/blacklinux --xbe /tmp/opencode/boot.xbe --probe --timeout 3 --trace
```

Fixture ini image XBE sintetis (16 KB) yang kodenya **benar-benar dieksekusi**: ia memanggil stub
kernel dari game code, memasang frame SEH-nya sendiri, raise + unwind exception, dan menulis-baca
TLS slot lewat `fs:[0xE10+4*slot]`. Yang terbukti di sini: fs patch, import resolution, boot thread
beserta KTHREAD/TLS-nya, dan SEH. Yang tidak terbukti: apa pun yang hanya ada di image asli
(bentuk TLS/entry/section yang sebenarnya, SHA-256 build yang dikenal, `_except_handler3`).

Byte fixture-nya dihasilkan dari `tests/fixture_gen/boot.s` oleh assembler, lalu ditulis ke
`tests/xbe_fixture.h` oleh `gen.sh` sendiri:

```sh
cd loader/tests/fixture_gen && ./gen.sh
```

Jangan mengetik offset atau byte di tangan — `gen.sh` membacanya dari objdump, dan setiap perubahan
dirukun compile-time.

---

## 10. Setelah XBE masuk

Urutan yang sudah direncanakan:

1. `--probe --trace` → baca `UNIMPLEMENTED ... call #1` untuk ordinal apa pun, itu urutan kerja.
2. `k_dpc` (thread timer/DPC) — 6 stub terakhir, dan DPC sering dibutuhkan sebelum game bisa lanjut.
3. D3D8 di atas device Vulkan yang sudah ada: device object, resource, draw, hook `.d3d` di image.
4. Shader nv2a → SPIR-V (bagian yang benar-benar butuh image asli: format microcode-nya belum pernah dilihat).
5. Fase 2 (SDL2: window/input/audio) supaya ada jendela dan kontrol.
6. `KeSet/QueryBasePriorityThread` sudah selesai di `k_thread`.

Rencana lengkap: `.agents/plan.md`.
