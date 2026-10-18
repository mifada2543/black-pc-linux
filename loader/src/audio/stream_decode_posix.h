// Stream decoder: WMA/XMV decode for DirectSound streams.
// Wraps the FFmpeg decoder layer (audio/decoder_posix.cpp) to decode stream packets on the fly.
// Integrates with the audio_out mixer for playback.
//
// These declarations are NOT backed by a working implementation in a build without FFmpeg. decoder_posix
// guards on HAVE_FFMPEG_HEADERS and falls back to stubs that return null, and stream_decode_posix.cpp is
// only added to the build when HAVE_FFMPEG is true. So on this machine there is no object behind any
// declaration below -- there is no WMA audio decode and no XMV video decode, and a caller that obtained
// this header has no evidence either is available. Do not read the API surface as working playback.
#pragma once

#include "audio/decoder_posix.h"

#include <cstdint>

namespace stream_decode {

// Stream types
enum StreamType {
    STREAM_AUDIO_WMA = 1,  // WMA audio stream
    STREAM_VIDEO_XMV = 2,  // XMV video stream
};

// Stream handle (opaque to caller)
struct Stream {
    void *opaque;              // decoder-specific state
    StreamType type;
    uint32_t sample_rate;      // audio: 48000, video: fps
    uint32_t channels;         // audio: 2, video: N/A
    uint32_t width;            // video only
    uint32_t height;           // video only
    bool paused;
    bool eof;
};

// Open a stream for decoding.
// format_tag: WAVEFORMATEX.wFormatTag (0x161 = WMA, 0x163 = XMV)
// Returns null on failure.
Stream *stream_open(uint16_t format_tag, const uint8_t *extradata, uint32_t extradata_size);

// Decode one packet into output buffer.
// audio: out must be at least max_frames * channels * sizeof(float)
// video: out must be width * height * 4 (RGBA)
// Returns number of frames decoded (audio) or 1/0 (video), negative on error.
int stream_decode(Stream *s, const uint8_t *packet, uint32_t packet_size,
                  float *out_audio, uint32_t max_frames,
                  uint8_t *out_video_rgba);

// Submit decoded audio to mixer (audio_out::submit)
void stream_submit_audio(const float *lr, uint32_t frames);

// Get video frame as RGBA texture (for rendering)
// Returns true if new frame available
bool stream_get_video_frame(Stream *s, uint8_t *out_rgba);

// Pause/resume stream
void stream_pause(Stream *s, bool paused);

// Close and free stream
void stream_close(Stream *s);

// Stream callback type (matches DirectSound stream callback)
using StreamCallback = void (*)(void *context, void *packet_context, uint32_t status);

// Set callback for packet completion
void stream_set_callback(Stream *s, StreamCallback cb, void *context);

} // namespace stream_decode