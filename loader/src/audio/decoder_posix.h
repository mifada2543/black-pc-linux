// FFmpeg decoder wrapper for WMA (audio) and XMV (video).
//
// The Xbox stores audio in several shapes. The two this file knows about are:
//
//   * WMAv2/Pro packets, which arrive with a WAVEFORMATEX-style header that carries the block size and
//     the codec's extradata. The caller feeds one compressed packet at a time -- there is no container
//     to demux, because the game's DirectSound stream already carries the packet boundaries.
//   * XMV files, which are a container (FFmpeg calls it "xmv") holding WMV2 video. This game's XMV files
//     have no audio stream at all; audio lives in the game's own sound banks and reaches the mixer as
//     PCM/ADPCM, not as WMA. XMV is therefore opened and demuxed here, and the caller only asks for the
//     next frame.
//
// Both paths are packet-driven. The previous version of this file opened a decoder and then called
// avcodec_receive_frame without ever sending a packet, which cannot decode anything: the first call
// returns EAGAIN forever. That is why the library being installed would not have been enough.
#pragma once

#include <cstdint>

namespace decoder {

// Decoder result codes. Positive values are counts/frames.
enum Result {
    OK = 0,
    ERROR_INIT = -1,
    ERROR_DECODE = -2,
    ERROR_EOF = -3,
    ERROR_BUFFER = -4,
};

// The fields of WAVEFORMATEX that a WMA decoder needs, plus the codec's extradata.
// `format_tag` uses the WAVE_FORMAT_* values (0x161 = WMAv2, 0x162 = WMAPro).
struct AudioFormat {
    uint16_t format_tag = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint32_t avg_bytes_per_sec = 0;
    uint16_t block_align = 0;
    uint16_t bits_per_sample = 0;
    const uint8_t *extradata = nullptr;
    uint32_t extradata_size = 0;
};

// Audio decoder context (WMA). The output is always 48 kHz stereo float, because that is what the host
// mixer (audio/output_posix.h) consumes; resampling happens inside this layer.
struct AudioDecoder {
    void *opaque = nullptr; // internal FFmpeg context
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
};

// Initialize an audio decoder from the stream's format. Returns null on failure (and says why in the log).
AudioDecoder *audio_open(const AudioFormat &fmt);

// Feed one compressed packet and decode as much as fits. Packets are queued, so a caller that pushes
// packets faster than it drains samples (DirectSound callbacks do) never loses one. `packet` may be null
// with `packet_size == 0` to pull more output. Returns the number of stereo frames written to `out`
// (interleaved L/R), 0 when the decoder has nothing to give yet, or a negative error code (ERROR_EOF once
// the stream is drained).
int audio_decode(AudioDecoder *dec, const uint8_t *packet, uint32_t packet_size, float *out,
                 uint32_t max_frames);

// Mark the end of the packet stream. Packets already queued are still decoded before the decoder is
// flushed; the following audio_decode calls return the remaining samples and finally ERROR_EOF.
void audio_flush(AudioDecoder *dec);

// Close the decoder and free everything it owns.
void audio_close(AudioDecoder *dec);

// Video decoder context (XMV container, WMV2 video).
struct VideoDecoder {
    void *opaque = nullptr; // internal FFmpeg context
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps_num = 0;
    uint32_t fps_den = 0;
};

// Open an XMV/ASF file held in memory. Returns null on failure.
VideoDecoder *video_open(const uint8_t *data, uint32_t size);

// Open an XMV/ASF file from disk. Returns null on failure.
VideoDecoder *video_open_file(const char *path);

// Decode the next video frame into `out_rgba` (width * height * 4 bytes, tightly packed RGBA8).
// Returns 1 when a frame was written, 0 when more input is needed, ERROR_EOF at end of stream, or
// another negative code on failure.
int video_decode(VideoDecoder *dec, uint8_t *out_rgba);

// Close the decoder and free everything it owns.
void video_close(VideoDecoder *dec);

} // namespace decoder
