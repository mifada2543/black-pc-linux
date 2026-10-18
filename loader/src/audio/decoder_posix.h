// FFmpeg decoder wrapper for WMA (audio) and XMV (video).
// Uses libavcodec from FFmpeg (linked via libavcodec-dev:i386).
#pragma once

#include <cstdint>

namespace decoder {

// Decoder result codes
enum Result {
    OK = 0,
    ERROR_INIT = -1,
    ERROR_DECODE = -2,
    ERROR_EOF = -3,
    ERROR_BUFFER = -4,
};

// Audio decoder context (WMA)
struct AudioDecoder {
    void *opaque; // internal FFmpeg context
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
};

// Video decoder context (XMV)
struct VideoDecoder {
    void *opaque; // internal FFmpeg context
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;
    uint32_t fps_den;
};

// Initialize audio decoder (WMA). Returns null on failure.
AudioDecoder *audio_open(const uint8_t *data, uint32_t size);

// Decode next audio frame into output buffer (float interleaved L/R).
// Returns number of frames written, or negative error code.
int audio_decode(AudioDecoder *dec, float *out, uint32_t max_frames);

// Close audio decoder.
void audio_close(AudioDecoder *dec);

// Initialize video decoder (XMV). Returns null on failure.
VideoDecoder *video_open(const uint8_t *data, uint32_t size);

// Decode next video frame into RGBA buffer.
// out must be at least width * height * 4 bytes.
// Returns 1 if frame decoded, 0 if need more data, negative on error.
int video_decode(VideoDecoder *dec, uint8_t *out_rgba);

// Close video decoder.
void video_close(VideoDecoder *dec);

} // namespace decoder