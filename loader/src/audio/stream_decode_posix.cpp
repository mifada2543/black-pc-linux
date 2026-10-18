// Stream decoder implementation: WMA/XMV decode for DirectSound streams.
#include "audio/stream_decode_posix.h"

#include "audio/decoder_posix.h"
#include "audio/output_posix.h"

#include "platform/posix/log.h"

#include <cstring>
#include <vector>

namespace stream_decode {

namespace {

struct StreamImpl {
    StreamType type;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t width;
    uint32_t height;
    bool paused;
    bool eof;

    // Audio decoder
    decoder::AudioDecoder *audio_dec = nullptr;
    std::vector<float> audio_buffer;
    uint32_t audio_buffer_pos = 0;
    uint32_t audio_buffer_frames = 0;

    // Video decoder
    decoder::VideoDecoder *video_dec = nullptr;
    std::vector<uint8_t> video_frame_rgba;
    bool video_frame_ready = false;

    // Callback
    StreamCallback callback = nullptr;
    void *callback_context = nullptr;
};

StreamType format_tag_to_type(uint16_t format_tag)
{
    // WAVEFORMATEX.wFormatTag values
    switch (format_tag) {
        case 0x161: // WAVE_FORMAT_WMAUDIO2 (WMA)
        case 0x162: // WAVE_FORMAT_WMAUDIO3
            return STREAM_AUDIO_WMA;
        case 0x163: // WAVE_FORMAT_XMV (Xbox Media Video)
            return STREAM_VIDEO_XMV;
        default:
            return STREAM_AUDIO_WMA; // default to audio
    }
}

Stream *stream_open(uint16_t format_tag, const uint8_t *extradata, uint32_t extradata_size)
{
    StreamType type = format_tag_to_type(format_tag);
    StreamImpl *impl = new StreamImpl();
    impl->type = type;
    impl->paused = false;
    impl->eof = false;

    if (type == STREAM_AUDIO_WMA) {
        impl->audio_dec = decoder::audio_open(extradata, extradata_size);
        if (!impl->audio_dec) {
            logf("[stream] Failed to open WMA audio decoder");
            delete impl;
            return nullptr;
        }
        impl->sample_rate = impl->audio_dec->sample_rate;
        impl->channels = impl->audio_dec->channels;
        impl->audio_buffer.resize(480 * 2); // 1 block = 480 frames stereo
        logf("[stream] WMA audio stream opened: %u Hz, %u ch", impl->sample_rate, impl->channels);
    } else if (type == STREAM_VIDEO_XMV) {
        impl->video_dec = decoder::video_open(extradata, extradata_size);
        if (!impl->video_dec) {
            logf("[stream] Failed to open XMV video decoder");
            delete impl;
            return nullptr;
        }
        impl->width = impl->video_dec->width;
        impl->height = impl->video_dec->height;
        impl->sample_rate = impl->video_dec->fps_num ? impl->video_dec->fps_num : 30;
        impl->channels = 0;
        impl->video_frame_rgba.resize(impl->width * impl->height * 4);
        logf("[stream] XMV video stream opened: %ux%u @ %u fps", impl->width, impl->height, impl->sample_rate);
    }

    Stream *stream = new Stream();
    stream->opaque = impl;
    stream->type = type;
    stream->sample_rate = impl->sample_rate;
    stream->channels = impl->channels;
    stream->width = impl->width;
    stream->height = impl->height;
    stream->paused = false;
    stream->eof = false;

    logf("[stream] Stream opened: type=%u, sample_rate=%u, ch=%u", type, stream->sample_rate, stream->channels);
    return stream;
}

int stream_decode(Stream *s, const uint8_t *packet, uint32_t packet_size,
                  float *out_audio, uint32_t max_frames,
                  uint8_t *out_video_rgba)
{
    if (!s || !s->opaque) return -1;
    StreamImpl *impl = static_cast<StreamImpl *>(s->opaque);

    if (impl->paused) return 0;
    if (impl->eof) return -1;

    if (impl->type == STREAM_AUDIO_WMA) {
        if (!impl->audio_dec) return -1;

        // Use audio buffer as intermediate
        int frames = decoder::audio_decode(impl->audio_dec, impl->audio_buffer.data(), max_frames);
        if (frames < 0) {
            impl->eof = true;
            return frames;
        }
        impl->audio_buffer_pos = 0;
        impl->audio_buffer_frames = frames;

        // Copy to output
        uint32_t copy_frames = std::min<uint32_t>(frames, max_frames);
        std::memcpy(out_audio, impl->audio_buffer.data(), copy_frames * 2 * sizeof(float));
        return copy_frames;
    } else if (impl->type == STREAM_VIDEO_XMV) {
        if (!impl->video_dec) return -1;

        // For video, we just decode and store the frame
        int ret = decoder::video_decode(impl->video_dec, impl->video_frame_rgba.data());
        if (ret == 1) {
            impl->video_frame_ready = true;
            if (out_video_rgba) {
                std::memcpy(out_video_rgba, impl->video_frame_rgba.data(), impl->width * impl->height * 4);
            }
            return 1;
        } else if (ret < 0) {
            impl->eof = true;
            return ret;
        }
        return 0; // need more data
    }
    return -1;
}

void stream_submit_audio(const float *lr, uint32_t frames)
{
    audio_out::submit(lr);
}

bool stream_get_video_frame(Stream *s, uint8_t *out_rgba)
{
    if (!s || !s->opaque) return false;
    StreamImpl *impl = static_cast<StreamImpl *>(s->opaque);

    if (!impl->video_frame_ready) return false;
    if (out_rgba) {
        std::memcpy(out_rgba, impl->video_frame_rgba.data(), impl->width * impl->height * 4);
    }
    impl->video_frame_ready = false; // consume
    return true;
}

void stream_pause(Stream *s, bool paused)
{
    if (!s || !s->opaque) return;
    static_cast<StreamImpl *>(s->opaque)->paused = paused;
}

void stream_close(Stream *s)
{
    if (!s) return;
    StreamImpl *impl = static_cast<StreamImpl *>(s->opaque);
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
    static_cast<StreamImpl *>(s->opaque)->callback = cb;
    static_cast<StreamImpl *>(s->opaque)->callback_context = context;
}

} // namespace stream_decode
} // namespace