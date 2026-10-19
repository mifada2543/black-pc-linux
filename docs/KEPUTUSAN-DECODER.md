# Keputusan decoder audio/video (A9)

19 Oktober 2026

Dokumen ini menjawab tiga hal: kenapa A9 sebelumnya hanya berupa stub, jalur apa yang dipilih, dan bukti
apa yang sudah dijalankan di mesin ini. Yang **belum** termasuk di sini: memutar suara dan video di dalam
game. Itu bagian lain (lihat bagian 6).

---

## 1. Keputusan

| Pertanyaan | Keputusan |
|---|---|
| Pustaka | FFmpeg 6.1 (`libavcodec` 60, `libavformat` 60), **ditautkan dinamis** dari paket distro |
| Lisensi | FFmpeg build LGPL-2.1+; project GPL-2.0-or-later; tidak ada komponen `--enable-gpl` yang dipakai |
| Audio | WMAv2 (`0x161`) dan WMAPro (`0x162`) lewat `avcodec_send_packet`/`avcodec_receive_frame`, keluaran **48 kHz stereo float** |
| Video | Kontainer **XMV** lewat demuxer `xmv` milik FFmpeg, video **WMV2** (fallback WMV3/VC1), keluaran **RGBA8** |
| Sampel uji | Sintetis: sinus 440 Hz (kiri) + 660 Hz (kanan) di-encode dengan encoder `wmav2` lokal, lalu didekode dan dibandingkan |
| Berkas game | Tidak pernah disalin ke repo; uji `.xmv` hanya dijalankan bila `game/videos/` ada, dan dilewati bila tidak |

## 2. Kenapa sebelumnya stub

Ada **dua** sebab, dan yang kedua lebih penting daripada yang pertama.

**Pertama, paket i386 tidak terpasang.** Build daftar `find_path`/`find_library` mencari
`libavcodec-dev:i386`; di mesin ini yang terpasang hanya versi `amd64`, yang tidak bisa dipakai proses
32-bit. Tanpa header, `decoder_posix.cpp` mengambil jalur stub: semua `*_open` mengembalikan `null`, dan
`stream_decode_posix.cpp` sengaja tidak ikut dikompilasi supaya tidak membungkus stub menjadi API streaming
yang terlihat hidup.

**Kedua, kode "asli"-nya pun tidak bisa mendekode.** Lihat riwayat berkasnya sebelum perubahan ini:

- `audio_decode()` tidak punya parameter paket sama sekali. Ia hanya memanggil `avcodec_receive_frame`;
  `avcodec_send_packet` tidak pernah dipanggil. Dekoder tanpa paket tidak pernah mengeluarkan frame —
  panggilan pertama mengembalikan `EAGAIN` selamanya.
- `video_open()` memilih dekoder `AV_CODEC_ID_WMV3` lalu `VC1`, padahal video game ini adalah **WMV2**
  (bukti di bagian 3). Bahkan bila pustakanya terpasang, `video_decode` akan gagal.
- Tidak ada demuxer: XMV adalah kontainer, bukan aliran paket mentah.
- Tidak ada satu pun pemanggil. Tidak ada jalur yang menyambungkan dekoder ke DirectSound atau ke layar.

Jadi jawaban atas "tidak bisakah diperbaiki?" adalah **bisa**, tetapi memasang paketnya saja tidak cukup:
kode dekodernya harus ditulis ulang menjadi packet-driven, dan itu yang sekarang ada di repo.

## 3. Fakta dari berkas game (diukur, bukan diingat)

```
$ ffprobe -show_entries stream=codec_name,codec_type,width,height  game/videos/02_n.xmv
codec_name=wmv2
codec_type=video
width=640
height=480

$ ffprobe -show_entries format=nb_streams -of default game/videos/02_n.xmv
nb_streams=1
```

- Semua `.xmv` yang diperiksa (02, 13, dan lainnya) berisi **satu stream video WMV2 640x480**; tidak ada
  stream audio di dalamnya.
- Tidak ada satu pun berkas `.wma` di dataset. Suara game berbentuk bank `.awd`/`.bks`/`.ssh` dan RIFF
  Xbox ADPCM (`.dav`), bukan WMA. Artinya jalur WMA di A9 **tidak dipakai game ini**; ia tetap
  diimplementasikan karena gate A9 meminta satu tes dekoder yang lulus, dan karena XDK game menyertakan
  `WMADEC` yang mungkin dipakai build lain.
- `ffmpeg -demuxers | grep xmv` → `xmv  Microsoft XMV`; `ffmpeg -codecs | grep wma` → `wmav2` ada.

## 4. Paket per distro

Cek ketersediaan (bukan pemasangan) di Ubuntu 24.04 "noble" / Linux Mint 22 dengan
`apt-get download --print-uris`:

```
sudo apt install libavcodec-dev:i386 libavformat-dev:i386 libavutil-dev:i386 \
                 libswresample-dev:i386 libswscale-dev:i386
```

Kelima paket di atas punya kandidat `7:6.1.1-3ubuntu5` di `noble/universe` (`mirror.biznetgio.com`).
`libavutil-dev:i386` dan `libswresample-dev:i386` ikut sebagai dependensi, tetapi ditulis eksplisit supaya
perintahnya jelas. Distro lain (Fedora, Arch) **belum diverifikasi** di mesin ini; masuk daftar A11.

Setelah paket terpasang, `cmake --preset linux` menemukannya sendiri lewat `find_path`/`find_library` di
`/usr/include/i386-linux-gnu` dan `/usr/lib/i386-linux-gnu`, lalu mendaftarkan tes `decoder` sebagai suite
ke-31.

## 5. Verifikasi yang sudah dijalankan

Mesin ini tidak boleh memasang paket (tanpa `sudo`), jadi FFmpeg 6.1.1 i386 dibangun **minimal dan statis**
ke `/tmp/ffmpeg-i386` khusus untuk menjalankan tes — hanya komponen yang dibutuhkan:
`--disable-everything --enable-decoder=wmav2,wmv2,wmv3,vc1 --enable-encoder=wmav2
--enable-demuxer=xmv,asf --disable-x86asm`. Hasil build itu tidak masuk repo dan tidak menggantikan paket
distro; yang dibuktikan adalah kode di repo ini terhadap FFmpeg versi yang sama persis dengan paket distro
(6.1.1).

```
$ ./test_decoder                     # build dengan FFmpeg
WMAv2 decode round trip
  ok     the encoder produced packets / stereo / 44.1 kHz
  ok     the decoder opens the stream's format
  ok     the decoder resamples to the host mixer's 48 kHz
  (decoded 11 packets into 22274 frames of 48 kHz stereo)
  ok     the decoder produced about half a second of audio
  ok     the decoded signal has the amplitude that went in
  ok     the left channel is 440 Hz
  ok     the right channel is 660 Hz
  ok     the two channels are decoded independently
XMV video decode
  ok     the decoder opens a real XMV file from memory
  ok     the video is 640x480
  ok     the frame rate came through
  ok     the first frame decodes
  ok     the frames are images, not one flat colour
  ok     the same file opens through the path API
23 checks, 0 failures
```

Pembanding independen: ffmpeg CLI (amd64) mendekode berkas WMA yang sama menjadi **22.292** frame; selisih 18
frame adalah priming resampler, bukan kehilangan data.

```
$ ctest --test-dir build/ffmpeg   → 100% tests passed, 0 failed out of 31   (0 warning saat build)
$ ctest --preset linux            → 100% tests passed, 0 failed out of 30   (tanpa FFmpeg, stub jujur)
```

Ringkasnya: yang bisa dibuktikan **sudah** dibuktikan; yang tersisa hanyalah pemasangan paket distro oleh
pengguna, lalu build ulang.

## 6. Batas A9 (yang sengaja tidak diklaim)

- **Bukan playback in-game.** A9 hanya menyediakan dekoder dan API-nya (`decoder_posix`,
  `stream_decode_posix`). Yang menyambungkannya ke DirectSound/mixer belum diporting dari arsip Windows
  (`windows/src/audio/dsound.cpp`, mixer PCM/ADPCM + bin routing). Itu pekerjaan audio terpisah; tanpa itu
  game tetap tanpa suara meskipun FFmpeg terpasang.
- **XMV ke layar = G6.** Tes membuktikan frame WMV2 didekode menjadi RGBA; menampilkannya lewat pipeline
  Vulkan adalah item G6, bukan A9.
- **WMA bukan jalur suara game ini.** Bank suara game adalah PCM/ADPCM (bagian 3); dekoder WMA ada karena
  kontrak A9, bukan karena dibutuhkan oleh `Black` versi ini.
- **Kualitas**: tes memakai sinus, bukan telinga. Tidak ada klaim soal latency atau kualitas resampling
  selain "energi dan frekuensi bertahan".

## 7. Catatan teknis yang ditemukan saat memperbaiki

- **Header FFmpeg adalah C murni.** Header publiknya tidak punya penjaga `extern "C"`; dari C++ harus
  dibungkus sendiri, kalau tidak linker mencari nama yang di-mangle (`avcodec_find_encoder(AVCodecID)` alih-alih
  `avcodec_find_encoder`) dan gagal di taut. Ini penyebab kegagalan taut pertama saat pengerjaan.
- **Paket harus diantre, bukan dijatuhkan.** Callback DirectSound mendorong paket lebih cepat daripada
  mixer menyerap sampel; `decoder_posix` menyimpan paket dalam antrean (`std::deque`) supaya tidak ada paket
  yang hilang saat `avcodec_send_packet` mengembalikan `EAGAIN`.
- **Flush itu eksplisit.** Antrean kosong tidak berarti akhir aliran; `audio_flush()` yang menandainya.
  Sebelumnya loop internal salah mengirim `NULL` setiap antrean kosong sehingga paket berikutnya terbuang.
- **`libavformat` dan `libavcodec` saling merujuk** (`av_parser_*` ada di `libavcodec`, dipanggil dari
  demuxer `libavformat`). Untuk taut statis, kedua arsip dibungkus `-Wl,--start-group ... --end-group`.
  Untuk pustaka dinamis distro, urutan tidak masalah.
- **Rasio sampel**: WMA 44,1 kHz → mixer host 48 kHz; resampling dilakukan di lapisan dekoder, sehingga
  pemanggil selalu menerima 48 kHz stereo.
