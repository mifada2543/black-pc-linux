// FFmpeg decoder wrapper implementation for WMA (audio) and XMV (video).
//
// This file is deliberately packet-driven. A decoder that never receives a packet cannot produce a
// frame, and the earlier version of this file did exactly that: it opened libavcodec, then called
// avcodec_receive_frame in a loop. Every call returned EAGAIN, so even with the i386 package installed
// the result was silence. The tests in tests/test_decoder.cpp encode a WMA packet stream and decode it
// back, which is what makes this path a decoder rather than a symbol table.
//
// XMV is a container. FFmpeg's "xmv" demuxer hands us WMV2 packets (not WMV3/VC1: ffprobe on this game's
// videos reports wmv2 at 640x480), and libavcodec turns them into frames. The container carries no audio
// stream in this game; sound effects and music live in the game's sound banks and reach DirectSound as
// PCM/ADPCM, so there is nothing to decode here for those.
#include "audio/decoder_posix.h"

#include "platform/posix/log.h"

#if defined(HAVE_FFMPEG) || defined(__has_include)
#if __has_include(<libavcodec/avcodec.h>)
#define HAVE_FFMPEG_HEADERS 1
// FFmpeg's public headers are plain C and do not carry extern "C" guards, so C++ has to provide them.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#else
#define HAVE_FFMPEG_HEADERS 0
#endif
#else
#define HAVE_FFMPEG_HEADERS 0
#endif

#include <algorithm>
#include <cstring>
#include <deque>
#include <vector>

namespace decoder {

#if HAVE_FFMPEG_HEADERS

namespace {

// The host mixer is fixed at 48 kHz stereo float (audio/output_posix.h), so everything a game stream
// produces is converted to that here rather than in the mixer.
constexpr uint32_t OUT_RATE = 48000;
constexpr uint16_t OUT_CHANNELS = 2;

struct AudioDecoderImpl {
    AVCodecContext *codec_ctx = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *pkt = nullptr;
    SwrContext *swr = nullptr;

    // Compressed packets handed over by the caller, oldest first. A packet arrives from the game's
    // DirectSound callback whether or not the mixer has drained what the last one produced, so the
    // wrapper has to hold on to them rather than dropping any: a dropped packet is a hole in the audio.
    std::deque<std::vector<uint8_t>> queue;

    // Decoded frames waiting for the caller. Interleaved stereo floats.
    std::vector<float> pending;
    size_t pending_pos = 0; // in frames

    bool flush_requested = false;
    bool flush_sent = false;
    bool eof = false;
};

struct VideoDecoderImpl {
    AVFormatContext *fmt = nullptr;
    AVIOContext *avio = nullptr;
    AVCodecContext *codec = nullptr;
    AVFrame *frame = nullptr;
    AVFrame *rgba = nullptr;
    AVPacket *pkt = nullptr;
    SwsContext *sws = nullptr;
    int stream_index = -1;

    // Only used by video_open(data, size): a private copy of the caller's bytes, because an AVIOContext
    // must keep reading after video_open returns.
    std::vector<uint8_t> input;
    size_t read_pos = 0;

    bool demux_eof = false;
    bool flush_sent = false;
    bool decoder_eof = false;
};

AVCodecID audio_codec_id(uint16_t format_tag)
{
    switch (format_tag) {
        case 0x0161: return AV_CODEC_ID_WMAV2;      // WAVE_FORMAT_WMAUDIO2
        case 0x0162: return AV_CODEC_ID_WMAPRO;     // WAVE_FORMAT_WMAUDIO3
        case 0x0163: return AV_CODEC_ID_WMALOSSLESS; // WAVE_FORMAT_WMALOSSLESS
        default: return AV_CODEC_ID_NONE;
    }
}

void audio_cleanup(AudioDecoderImpl *impl)
{
    if (impl->swr) swr_free(&impl->swr);
    if (impl->frame) av_frame_free(&impl->frame);
    if (impl->pkt) av_packet_free(&impl->pkt);
    if (impl->codec_ctx) avcodec_free_context(&impl->codec_ctx);
}

size_t audio_pending_frames(const AudioDecoderImpl *impl)
{
    return impl->pending.size() / OUT_CHANNELS - impl->pending_pos;
}

uint32_t audio_take_pending(AudioDecoderImpl *impl, float *out, uint32_t max_frames)
{
    const size_t available = audio_pending_frames(impl);
    const size_t take = std::min<size_t>(available, max_frames);
    if (take == 0) return 0;
    std::memcpy(out, impl->pending.data() + impl->pending_pos * OUT_CHANNELS,
                take * OUT_CHANNELS * sizeof(float));
    impl->pending_pos += take;
    if (impl->pending_pos >= impl->pending.size() / OUT_CHANNELS) {
        impl->pending.clear();
        impl->pending_pos = 0;
    }
    return static_cast<uint32_t>(take);
}

// Pull every frame the decoder can produce right now into `pending`, resampled to 48 kHz stereo float.
// Returns false only on a real decode error; EAGAIN and EOF are normal states.
bool audio_pump(AudioDecoderImpl *impl)
{
    for (;;) {
        const int ret = avcodec_receive_frame(impl->codec_ctx, impl->frame);
        if (ret == AVERROR(EAGAIN)) return true;
        if (ret == AVERROR_EOF) {
            impl->eof = true;
            return true;
        }
        if (ret < 0) {
            logf("[decoder] WMA decode error: %d", ret);
            return false;
        }

        const int out_capacity = swr_get_out_samples(impl->swr, impl->frame->nb_samples);
        if (out_capacity < 0) {
            logf("[decoder] audio resampler refused a frame: %d", out_capacity);
            return false;
        }
        const size_t old_size = impl->pending.size();
        impl->pending.resize(old_size + static_cast<size_t>(out_capacity) * OUT_CHANNELS);
        uint8_t *out_data[1] = {reinterpret_cast<uint8_t *>(impl->pending.data() + old_size)};
        const int converted = swr_convert(impl->swr, out_data, out_capacity,
                                          const_cast<const uint8_t **>(impl->frame->extended_data),
                                          impl->frame->nb_samples);
        if (converted < 0) {
            logf("[decoder] audio resample error: %d", converted);
            impl->pending.resize(old_size);
            return false;
        }
        impl->pending.resize(old_size + static_cast<size_t>(converted) * OUT_CHANNELS);
    }
}

void video_cleanup(VideoDecoderImpl *impl)
{
    if (impl->sws) sws_freeContext(impl->sws);
    if (impl->rgba) av_frame_free(&impl->rgba);
    if (impl->frame) av_frame_free(&impl->frame);
    if (impl->pkt) av_packet_free(&impl->pkt);
    if (impl->codec) avcodec_free_context(&impl->codec);
    if (impl->fmt) {
        if (impl->avio) {
            av_freep(&impl->avio->buffer);
            avio_context_free(&impl->avio);
            impl->fmt->pb = nullptr;
        }
        avformat_close_input(&impl->fmt);
    }
}

int mem_read(void *opaque, uint8_t *buf, int buf_size)
{
    auto *impl = static_cast<VideoDecoderImpl *>(opaque);
    const size_t remaining = impl->input.size() - impl->read_pos;
    const size_t n = std::min<size_t>(remaining, static_cast<size_t>(buf_size));
    if (n == 0) return AVERROR_EOF;
    std::memcpy(buf, impl->input.data() + impl->read_pos, n);
    impl->read_pos += n;
    return static_cast<int>(n);
}

int64_t mem_seek(void *opaque, int64_t offset, int whence)
{
    auto *impl = static_cast<VideoDecoderImpl *>(opaque);
    if (whence == AVSEEK_SIZE) return static_cast<int64_t>(impl->input.size());
    int64_t base = 0;
    switch (whence & ~AVSEEK_FORCE) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = static_cast<int64_t>(impl->read_pos); break;
        case SEEK_END: base = static_cast<int64_t>(impl->input.size()); break;
        default: return AVERROR(EINVAL);
    }
    const int64_t target = base + offset;
    if (target < 0 || target > static_cast<int64_t>(impl->input.size())) return AVERROR(EINVAL);
    impl->read_pos = static_cast<size_t>(target);
    return target;
}

VideoDecoder *video_open_impl(VideoDecoderImpl *impl, const char *path)
{
    impl->fmt = avformat_alloc_context();
    if (!impl->fmt) {
        logf("[decoder] Failed to allocate XMV format context");
        video_cleanup(impl);
        delete impl;
        return nullptr;
    }

    if (!path) {
        uint8_t *iobuf = static_cast<uint8_t *>(av_malloc(4096));
        if (!iobuf) {
            logf("[decoder] Failed to allocate XMV I/O buffer");
            video_cleanup(impl);
            delete impl;
            return nullptr;
        }
        impl->avio = avio_alloc_context(iobuf, 4096, 0, impl, mem_read, nullptr, mem_seek);
        if (!impl->avio) {
            av_free(iobuf);
            logf("[decoder] Failed to allocate XMV I/O context");
            video_cleanup(impl);
            delete impl;
            return nullptr;
        }
        impl->fmt->pb = impl->avio;
        impl->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    }

    int ret = avformat_open_input(&impl->fmt, path, nullptr, nullptr);
    if (ret < 0) {
        logf("[decoder] Could not open XMV input (%s): %d", path ? path : "memory", ret);
        video_cleanup(impl);
        delete impl;
        return nullptr;
    }

    ret = avformat_find_stream_info(impl->fmt, nullptr);
    if (ret < 0) {
        logf("[decoder] Could not read XMV stream information: %d", ret);
        video_cleanup(impl);
        delete impl;
        return nullptr;
    }

    const AVCodec *codec = nullptr;
    impl->stream_index = av_find_best_stream(impl->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (impl->stream_index < 0 || !codec) {
        logf("[decoder] XMV input has no decodable video stream");
        video_cleanup(impl);
        delete impl;
        return nullptr;
    }

    AVStream *stream = impl->fmt->streams[impl->stream_index];
    impl->codec = avcodec_alloc_context3(codec);
    if (!impl->codec || avcodec_parameters_to_context(impl->codec, stream->codecpar) < 0 ||
        avcodec_open2(impl->codec, codec, nullptr) < 0) {
        logf("[decoder] Could not open XMV video decoder '%s'", codec->name);
        video_cleanup(impl);
        delete impl;
        return nullptr;
    }

    impl->frame = av_frame_alloc();
    impl->rgba = av_frame_alloc();
    impl->pkt = av_packet_alloc();
    if (!impl->frame || !impl->rgba || !impl->pkt) {
        logf("[decoder] Failed to allocate XMV decode state");
        video_cleanup(impl);
        delete impl;
        return nullptr;
    }

    const AVCodecParameters *par = stream->codecpar;
    const AVRational fps = av_guess_frame_rate(impl->fmt, stream, nullptr);

    VideoDecoder *dec = new VideoDecoder();
    dec->opaque = impl;
    dec->width = impl->codec->width ? impl->codec->width : par->width;
    dec->height = impl->codec->height ? impl->codec->height : par->height;
    dec->fps_num = fps.num ? fps.num : 30;
    dec->fps_den = fps.den ? fps.den : 1;

    logf("[decoder] XMV opened: %s, %ux%u @ %u/%u fps", codec->name, dec->width, dec->height,
         dec->fps_num, dec->fps_den);
    return dec;
}

} // namespace

AudioDecoder *audio_open(const AudioFormat &fmt)
{
    const AVCodecID id = audio_codec_id(fmt.format_tag);
    if (id == AV_CODEC_ID_NONE) {
        logf("[decoder] unsupported audio format tag 0x%04x", fmt.format_tag);
        return nullptr;
    }

    const AVCodec *codec = avcodec_find_decoder(id);
    if (!codec) {
        logf("[decoder] libavcodec has no decoder for format tag 0x%04x", fmt.format_tag);
        return nullptr;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        logf("[decoder] Failed to allocate WMA codec context");
        return nullptr;
    }

    ctx->sample_rate = static_cast<int>(fmt.sample_rate);
    ctx->block_align = fmt.block_align;
    ctx->bit_rate = static_cast<int64_t>(fmt.avg_bytes_per_sec) * 8;
    av_channel_layout_default(&ctx->ch_layout, fmt.channels);
    if (fmt.extradata && fmt.extradata_size) {
        ctx->extradata = static_cast<uint8_t *>(
            av_mallocz(fmt.extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!ctx->extradata) {
            avcodec_free_context(&ctx);
            logf("[decoder] Failed to copy WMA extradata");
            return nullptr;
        }
        std::memcpy(ctx->extradata, fmt.extradata, fmt.extradata_size);
        ctx->extradata_size = static_cast<int>(fmt.extradata_size);
    }

    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        logf("[decoder] Failed to open WMA decoder");
        return nullptr;
    }

    AudioDecoderImpl *impl = new AudioDecoderImpl();
    impl->codec_ctx = ctx;
    impl->frame = av_frame_alloc();
    impl->pkt = av_packet_alloc();
    if (!impl->frame || !impl->pkt) {
        audio_cleanup(impl);
        delete impl;
        return nullptr;
    }

    const int in_rate = ctx->sample_rate ? ctx->sample_rate : static_cast<int>(fmt.sample_rate);
    const int in_channels = ctx->ch_layout.nb_channels ? ctx->ch_layout.nb_channels : fmt.channels;
    if (in_rate <= 0 || in_channels <= 0) {
        logf("[decoder] WMA format has no usable rate/channel count");
        audio_cleanup(impl);
        delete impl;
        return nullptr;
    }

    AVChannelLayout in_layout, out_layout;
    av_channel_layout_default(&in_layout, in_channels);
    av_channel_layout_default(&out_layout, OUT_CHANNELS);
    const int swr_ret = swr_alloc_set_opts2(&impl->swr, &out_layout, AV_SAMPLE_FMT_FLT, OUT_RATE,
                                            &in_layout, ctx->sample_fmt, in_rate, 0, nullptr);
    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);
    if (swr_ret < 0 || !impl->swr || swr_init(impl->swr) < 0) {
        logf("[decoder] Failed to initialize audio resampler");
        audio_cleanup(impl);
        delete impl;
        return nullptr;
    }

    AudioDecoder *dec = new AudioDecoder();
    dec->opaque = impl;
    dec->sample_rate = OUT_RATE;
    dec->channels = OUT_CHANNELS;
    dec->bits_per_sample = 16;

    logf("[decoder] WMA audio decoder opened: %s, %d Hz %d ch -> %u Hz stereo", codec->name, in_rate,
         in_channels, OUT_RATE);
    return dec;
}

int audio_decode(AudioDecoder *dec, const uint8_t *packet, uint32_t packet_size, float *out,
                 uint32_t max_frames)
{
    if (!dec || !dec->opaque || !out) return ERROR_BUFFER;

    AudioDecoderImpl *impl = static_cast<AudioDecoderImpl *>(dec->opaque);

    if (packet && packet_size) {
        impl->queue.emplace_back(packet, packet + packet_size);
    }

    if (impl->eof && impl->queue.empty() && audio_pending_frames(impl) == 0) return ERROR_EOF;

    // Push queued packets into the decoder and pull frames out until either the caller's block is full or
    // there is genuinely nothing more to do. avcodec_send_packet refuses with EAGAIN while the decoder is
    // holding frames it has not handed over; that is not an error, it just means receive first.
    for (;;) {
        if (!impl->queue.empty()) {
            const std::vector<uint8_t> &front = impl->queue.front();
            if (av_new_packet(impl->pkt, static_cast<int>(front.size())) < 0) return ERROR_DECODE;
            std::memcpy(impl->pkt->data, front.data(), front.size());

            int ret = avcodec_send_packet(impl->codec_ctx, impl->pkt);
            if (ret == AVERROR(EAGAIN)) {
                if (!audio_pump(impl)) return ERROR_DECODE;
                ret = avcodec_send_packet(impl->codec_ctx, impl->pkt);
            }
            av_packet_unref(impl->pkt);
            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                logf("[decoder] WMA rejected a packet: %d", ret);
                return ERROR_DECODE;
            }
            if (ret >= 0) {
                impl->queue.pop_front();
            } else {
                return ERROR_DECODE; // still full after a receive: treat as broken rather than spin
            }
        }

        if (!audio_pump(impl)) return ERROR_DECODE;

        if (max_frames == 0 || audio_pending_frames(impl) >= max_frames) break;

        if (impl->queue.empty()) {
            if (impl->flush_requested && !impl->flush_sent) {
                const int ret = avcodec_send_packet(impl->codec_ctx, nullptr);
                impl->flush_sent = true;
                if (ret < 0 && ret != AVERROR_EOF) {
                    logf("[decoder] WMA flush failed: %d", ret);
                    return ERROR_DECODE;
                }
                continue;
            }
            break; // no input right now and no flush asked for: the caller may still push packets
        }
    }

    const uint32_t ready = audio_take_pending(impl, out, max_frames);
    if (ready == 0 && impl->eof) return ERROR_EOF;
    return static_cast<int>(ready);
}

void audio_flush(AudioDecoder *dec)
{
    if (!dec || !dec->opaque) return;
    static_cast<AudioDecoderImpl *>(dec->opaque)->flush_requested = true;
}

void audio_close(AudioDecoder *dec)
{
    if (!dec) return;
    if (dec->opaque) {
        auto *impl = static_cast<AudioDecoderImpl *>(dec->opaque);
        audio_cleanup(impl);
        delete impl;
    }
    delete dec;
}

VideoDecoder *video_open(const uint8_t *data, uint32_t size)
{
    if (!data || size == 0) {
        logf("[decoder] video_open called without data");
        return nullptr;
    }
    VideoDecoderImpl *impl = new VideoDecoderImpl();
    impl->input.assign(data, data + size);
    return video_open_impl(impl, nullptr);
}

VideoDecoder *video_open_file(const char *path)
{
    if (!path) return nullptr;
    VideoDecoderImpl *impl = new VideoDecoderImpl();
    return video_open_impl(impl, path);
}

int video_decode(VideoDecoder *dec, uint8_t *out_rgba)
{
    if (!dec || !dec->opaque || !out_rgba) return ERROR_BUFFER;

    VideoDecoderImpl *impl = static_cast<VideoDecoderImpl *>(dec->opaque);
    if (impl->decoder_eof) return ERROR_EOF;

    for (;;) {
        const int ret = avcodec_receive_frame(impl->codec, impl->frame);
        if (ret == 0) {
            if (!impl->sws) {
                impl->sws = sws_getContext(impl->frame->width, impl->frame->height,
                                           static_cast<AVPixelFormat>(impl->frame->format), dec->width,
                                           dec->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr,
                                           nullptr, nullptr);
                if (!impl->sws) {
                    logf("[decoder] Failed to initialize XMV scaler");
                    return ERROR_DECODE;
                }
                impl->rgba->format = AV_PIX_FMT_RGBA;
                impl->rgba->width = static_cast<int>(dec->width);
                impl->rgba->height = static_cast<int>(dec->height);
                if (av_frame_get_buffer(impl->rgba, 32) < 0) {
                    logf("[decoder] Failed to allocate XMV RGBA frame");
                    return ERROR_DECODE;
                }
            }

            sws_scale(impl->sws, impl->frame->data, impl->frame->linesize, 0, impl->frame->height,
                      impl->rgba->data, impl->rgba->linesize);

            const size_t row_bytes = static_cast<size_t>(dec->width) * 4;
            for (uint32_t y = 0; y < dec->height; y++) {
                std::memcpy(out_rgba + y * row_bytes, impl->rgba->data[0] + y * impl->rgba->linesize[0],
                            row_bytes);
            }
            return 1;
        }
        if (ret == AVERROR_EOF) {
            impl->decoder_eof = true;
            return ERROR_EOF;
        }
        if (ret != AVERROR(EAGAIN)) {
            logf("[decoder] XMV decode error: %d", ret);
            return ERROR_DECODE;
        }

        // The decoder wants another packet.
        if (impl->demux_eof) {
            if (impl->flush_sent) {
                impl->decoder_eof = true;
                return ERROR_EOF;
            }
            avcodec_send_packet(impl->codec, nullptr);
            impl->flush_sent = true;
            continue;
        }

        const int read_ret = av_read_frame(impl->fmt, impl->pkt);
        if (read_ret == AVERROR_EOF) {
            impl->demux_eof = true;
            continue;
        }
        if (read_ret < 0) {
            logf("[decoder] XMV read error: %d", read_ret);
            return ERROR_DECODE;
        }

        if (impl->pkt->stream_index == impl->stream_index) {
            const int send_ret = avcodec_send_packet(impl->codec, impl->pkt);
            av_packet_unref(impl->pkt);
            if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) {
                logf("[decoder] XMV rejected a packet: %d", send_ret);
                return ERROR_DECODE;
            }
        } else {
            av_packet_unref(impl->pkt);
        }
    }
}

void video_close(VideoDecoder *dec)
{
    if (!dec) return;
    if (dec->opaque) {
        auto *impl = static_cast<VideoDecoderImpl *>(dec->opaque);
        video_cleanup(impl);
        delete impl;
    }
    delete dec;
}

#else // !HAVE_FFMPEG_HEADERS

// Without the i386 development package there is no decoder to call. These are the truth, not a
// placeholder for one: every entry point says so in the log, and every open returns null.
AudioDecoder *audio_open(const AudioFormat &)
{
    logf("[decoder] FFmpeg headers are not available: audio decoder disabled");
    return nullptr;
}

int audio_decode(AudioDecoder *, const uint8_t *, uint32_t, float *, uint32_t) { return ERROR_INIT; }

void audio_flush(AudioDecoder *) {}

void audio_close(AudioDecoder *) {}

VideoDecoder *video_open(const uint8_t *, uint32_t)
{
    logf("[decoder] FFmpeg headers are not available: video decoder disabled");
    return nullptr;
}

VideoDecoder *video_open_file(const char *)
{
    logf("[decoder] FFmpeg headers are not available: video decoder disabled");
    return nullptr;
}

int video_decode(VideoDecoder *, uint8_t *) { return ERROR_INIT; }

void video_close(VideoDecoder *) {}

#endif // HAVE_FFMPEG_HEADERS

} // namespace decoder
