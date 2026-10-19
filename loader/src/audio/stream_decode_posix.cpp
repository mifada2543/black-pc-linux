// Stream decoder implementation: WMA/XMV decode for DirectSound-style streams.
//
// BUILT ONLY WHEN FFMPEG IS PRESENT. CMakeLists.txt adds this file to blackcore only inside
// `if(HAVE_FFMPEG)`. Without libavcodec-dev:i386 this translation unit is not compiled at all: the
// decoder layer below it (decoder_posix.cpp) builds, guards on HAVE_FFMPEG_HEADERS, and returns null
// from every open. Wrapping those nulls in a streaming API that produces no samples would read as
// "streaming is implemented", which is why this file is absent rather than stubbed.
#include "audio/stream_decode_posix.h"

#include "audio/decoder_posix.h"
#include "audio/output_posix.h"

#include "platform/posix/log.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace stream_decode {
namespace {

struct StreamImpl {
    StreamType type = STREAM_AUDIO_WMA;
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool paused = false;
    bool eof = false;

    decoder::AudioDecoder *audio_dec = nullptr;
    decoder::VideoDecoder *video_dec = nullptr;

    std::vector<uint8_t> video_frame_rgba;
    bool video_frame_ready = false;

    StreamCallback callback = nullptr;
    void *callback_context = nullptr;
};

} // namespace

Stream *stream_open_audio(const decoder::AudioFormat &fmt)
{
    decoder::AudioDecoder *dec = decoder::audio_open(fmt);
    if (!dec) {
        logf("[stream] Failed to open WMA audio decoder");
        return nullptr;
    }

    StreamImpl *impl = new StreamImpl();
    impl->type = STREAM_AUDIO_WMA;
    impl->audio_dec = dec;
    impl->sample_rate = dec->sample_rate;
    impl->channels = dec->channels;

    Stream *stream = new Stream();
    stream->opaque = impl;
    stream->type = STREAM_AUDIO_WMA;
    stream->sample_rate = impl->sample_rate;
    stream->channels = impl->channels;

    logf("[stream] WMA audio stream opened: %u Hz, %u ch", impl->sample_rate, impl->channels);
    return stream;
}

Stream *stream_open_video(const uint8_t *data, uint32_t size)
{
    decoder::VideoDecoder *dec = decoder::video_open(data, size);
    if (!dec) {
        logf("[stream] Failed to open XMV video decoder");
        return nullptr;
    }

    StreamImpl *impl = new StreamImpl();
    impl->type = STREAM_VIDEO_XMV;
    impl->video_dec = dec;
    impl->width = dec->width;
    impl->height = dec->height;
    impl->sample_rate = dec->fps_num ? dec->fps_num : 30;
    impl->video_frame_rgba.resize(static_cast<size_t>(impl->width) * impl->height * 4);

    Stream *stream = new Stream();
    stream->opaque = impl;
    stream->type = STREAM_VIDEO_XMV;
    stream->sample_rate = impl->sample_rate;
    stream->width = impl->width;
    stream->height = impl->height;

    logf("[stream] XMV video stream opened: %ux%u @ %u fps", impl->width, impl->height,
         impl->sample_rate);
    return stream;
}

int stream_decode(Stream *s, const uint8_t *packet, uint32_t packet_size, float *out_audio,
                  uint32_t max_frames, uint8_t *out_video_rgba)
{
    if (!s || !s->opaque) return -1;
    StreamImpl *impl = static_cast<StreamImpl *>(s->opaque);

    if (impl->paused) return 0;
    if (impl->eof) return decoder::ERROR_EOF;

    if (impl->type == STREAM_AUDIO_WMA) {
        if (!impl->audio_dec || !out_audio) return decoder::ERROR_BUFFER;
        const int frames = decoder::audio_decode(impl->audio_dec, packet, packet_size, out_audio,
                                                 max_frames);
        if (frames == decoder::ERROR_EOF) impl->eof = true;
        return frames;
    }

    if (impl->type == STREAM_VIDEO_XMV) {
        if (!impl->video_dec) return -1;
        const int ret = decoder::video_decode(impl->video_dec, impl->video_frame_rgba.data());
        if (ret == 1) {
            impl->video_frame_ready = true;
            if (out_video_rgba) {
                std::memcpy(out_video_rgba, impl->video_frame_rgba.data(),
                            static_cast<size_t>(impl->width) * impl->height * 4);
            }
            return 1;
        }
        if (ret == decoder::ERROR_EOF) impl->eof = true;
        return ret;
    }

    return -1;
}

void stream_flush_audio(Stream *s)
{
    if (!s || !s->opaque) return;
    StreamImpl *impl = static_cast<StreamImpl *>(s->opaque);
    if (impl->audio_dec) decoder::audio_flush(impl->audio_dec);
}

void stream_submit_audio(const float *lr, uint32_t frames)
{
    if (!lr || frames == 0) return;

    // audio_out consumes exactly one BLOCK (480 frames) per submit. Send whole blocks, then pad the
    // tail with silence rather than dropping it or handing the mixer a short buffer it cannot read.
    float block[audio_out::BLOCK * 2] = {};
    uint32_t offset = 0;
    while (offset < frames) {
        const uint32_t take = std::min<uint32_t>(audio_out::BLOCK, frames - offset);
        std::memcpy(block, lr + static_cast<size_t>(offset) * 2, take * 2 * sizeof(float));
        if (take < audio_out::BLOCK) {
            std::memset(block + take * 2, 0, (audio_out::BLOCK - take) * 2 * sizeof(float));
        }
        audio_out::submit(block);
        offset += take;
    }
}

bool stream_get_video_frame(Stream *s, uint8_t *out_rgba)
{
    if (!s || !s->opaque) return false;
    StreamImpl *impl = static_cast<StreamImpl *>(s->opaque);

    if (!impl->video_frame_ready) return false;
    if (out_rgba) {
        std::memcpy(out_rgba, impl->video_frame_rgba.data(),
                    static_cast<size_t>(impl->width) * impl->height * 4);
    }
    impl->video_frame_ready = false;
    return true;
}

void stream_pause(Stream *s, bool paused)
{
    if (!s || !s->opaque) return;
    static_cast<StreamImpl *>(s->opaque)->paused = paused;
    s->paused = paused;
}

void stream_close(Stream *s)
{
    if (!s) return;
    auto *impl = static_cast<StreamImpl *>(s->opaque);
    if (impl) {
        if (impl->audio_dec) decoder::audio_close(impl->audio_dec);
        if (impl->video_dec) decoder::video_close(impl->video_dec);
        delete impl;
    }
    delete s;
}

void stream_set_callback(Stream *s, StreamCallback cb, void *context)
{
    if (!s || !s->opaque) return;
    auto *impl = static_cast<StreamImpl *>(s->opaque);
    impl->callback = cb;
    impl->callback_context = context;
}

} // namespace stream_decode
