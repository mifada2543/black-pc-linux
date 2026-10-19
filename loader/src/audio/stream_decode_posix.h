// Stream decoder: WMA/XMV decode on top of audio/decoder_posix.
//
// A "stream" here is the shape the game's DirectSound stream uses: a compressed format (WMA) plus one
// packet at a time. Video is different -- XMV is a container, so the decoder owns the demuxer and the
// stream layer only asks for the next frame.
//
// This file is built only when FFmpeg is present (CMakeLists.txt adds it inside if(HAVE_FFMPEG)).
// Without the i386 development package the declarations below have no object behind them, and
// decoder_posix.cpp's open calls return null. That is the honest state: no WMA audio decode and no XMV
// video decode. Do not read this API surface as working playback on a build without FFmpeg.
#pragma once

#include "audio/decoder_posix.h"

#include <cstdint>

namespace stream_decode {

enum StreamType {
    STREAM_AUDIO_WMA = 1,
    STREAM_VIDEO_XMV = 2,
};

struct Stream {
    void *opaque = nullptr;
    StreamType type = STREAM_AUDIO_WMA;
    uint32_t sample_rate = 0; // audio: 48000 (the host mixer rate); video: fps
    uint32_t channels = 0;    // audio: 2; video: 0
    uint32_t width = 0;       // video only
    uint32_t height = 0;      // video only
    bool paused = false;
    bool eof = false;
};

// Open a WMA stream from the format the game's DirectSound stream carries.
Stream *stream_open_audio(const decoder::AudioFormat &fmt);

// Open an XMV video stream from a file held in memory.
Stream *stream_open_video(const uint8_t *data, uint32_t size);

// Decode one step.
//   audio: feed `packet`; writes up to max_frames stereo frames to out_audio.
//   video: `packet` is ignored (the container is demuxed by the decoder); writes one RGBA frame.
// Returns frames written (audio), 1/0 (video), or a negative error code.
int stream_decode(Stream *s, const uint8_t *packet, uint32_t packet_size, float *out_audio,
                  uint32_t max_frames, uint8_t *out_video_rgba);

// Mark the end of an audio stream. Queued packets are still decoded; later calls return the remaining
// samples and then ERROR_EOF. Mirrors decoder::audio_flush.
void stream_flush_audio(Stream *s);

// Submit decoded audio to the host mixer (audio_out::submit). Partial blocks are padded with silence.
void stream_submit_audio(const float *lr, uint32_t frames);

// Copy the most recently decoded video frame, if one is waiting. Returns true when it copied one.
bool stream_get_video_frame(Stream *s, uint8_t *out_rgba);

void stream_pause(Stream *s, bool paused);

void stream_close(Stream *s);

using StreamCallback = void (*)(void *context, void *packet_context, uint32_t status);

void stream_set_callback(Stream *s, StreamCallback cb, void *context);

} // namespace stream_decode
