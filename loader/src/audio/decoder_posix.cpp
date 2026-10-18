// FFmpeg decoder wrapper implementation for WMA and XMV.
// Uses libavcodec (linked via libavcodec-dev:i386).
#include "audio/decoder_posix.h"

#include "platform/posix/log.h"

#if defined(HAVE_FFMPEG) || defined(__has_include)
#if __has_include(<libavcodec/avcodec.h>)
#define HAVE_FFMPEG_HEADERS 1
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#else
#define HAVE_FFMPEG_HEADERS 0
#endif
#else
#define HAVE_FFMPEG_HEADERS 0
#endif

#include <cstring>

namespace decoder {

#if HAVE_FFMPEG_HEADERS

namespace {

// Internal audio decoder state
struct AudioDecoderImpl {
    AVCodecContext *codec_ctx = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *pkt = nullptr;
    SwrContext *swr = nullptr;
    uint8_t *extra_data = nullptr;
    int extra_size = 0;
    bool got_extradata = false;
    int64_t next_pts = 0;
};

// Internal video decoder state
struct VideoDecoderImpl {
    AVCodecContext *codec_ctx = nullptr;
    AVFrame *frame = nullptr;
    AVFrame *frame_rgba = nullptr;
    AVPacket *pkt = nullptr;
    SwsContext *sws = nullptr;
    uint8_t *extra_data = nullptr;
    int extra_size = 0;
    bool got_extradata = false;
    int64_t next_pts = 0;
};

AudioDecoderImpl *audio_impl_from_handle(AudioDecoder *h)
{
    return reinterpret_cast<AudioDecoderImpl *>(h->opaque);
}

VideoDecoderImpl *video_impl_from_handle(VideoDecoder *h)
{
    return reinterpret_cast<VideoDecoderImpl *>(h->opaque);
}

void audio_cleanup(AudioDecoderImpl *impl)
{
    if (impl->swr) swr_free(&impl->swr);
    if (impl->frame) av_frame_free(&impl->frame);
    if (impl->pkt) av_packet_free(&impl->pkt);
    if (impl->codec_ctx) avcodec_free_context(&impl->codec_ctx);
    if (impl->extra_data) av_free(impl->extra_data);
}

void video_cleanup(VideoDecoderImpl *impl)
{
    if (impl->sws) sws_freeContext(impl->sws);
    if (impl->frame_rgba) av_frame_free(&impl->frame_rgba);
    if (impl->frame) av_frame_free(&impl->frame);
    if (impl->pkt) av_packet_free(&impl->pkt);
    if (impl->codec_ctx) avcodec_free_context(&impl->codec_ctx);
    if (impl->extra_data) av_free(impl->extra_data);
}

} // namespace

AudioDecoder *audio_open(const uint8_t *data, uint32_t size)
{
    (void)data; (void)size;
    // Find WMA decoder
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_WMAV2);
    if (!codec) {
        logf("[decoder] WMAv2 decoder not found in libavcodec");
        return nullptr;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        logf("[decoder] Failed to allocate WMA codec context");
        return nullptr;
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

    AudioDecoder *dec = new AudioDecoder();
    dec->opaque = impl;
    dec->sample_rate = ctx->sample_rate ? ctx->sample_rate : 48000;
    dec->channels = ctx->channels ? ctx->channels : 2;
    dec->bits_per_sample = 16;

    logf("[decoder] WMA audio decoder opened: %u Hz, %u ch", dec->sample_rate, dec->channels);
    return dec;
}

int audio_decode(AudioDecoder *dec, float *out, uint32_t max_frames)
{
    if (!dec || !dec->opaque || !out) return ERROR_BUFFER;

    AudioDecoderImpl *impl = audio_impl_from_handle(dec);

    if (!impl->swr && impl->codec_ctx) {
        int64_t out_ch_layout = av_get_default_channel_layout(2);
        int64_t in_ch_layout = impl->codec_ctx->channel_layout;
        if (!in_ch_layout) in_ch_layout = av_get_default_channel_layout(impl->codec_ctx->channels);

        impl->swr = swr_alloc_set_opts(nullptr,
                                       out_ch_layout, AV_SAMPLE_FMT_FLT, impl->codec_ctx->sample_rate,
                                       in_ch_layout, impl->codec_ctx->sample_fmt, impl->codec_ctx->sample_rate,
                                       0, nullptr);
        if (!impl->swr || swr_init(impl->swr) < 0) {
            logf("[decoder] Failed to initialize audio resampler");
            return ERROR_DECODE;
        }
    }

    int frames_decoded = 0;
    while (frames_decoded < (int)max_frames) {
        int ret = avcodec_receive_frame(impl->codec_ctx, impl->frame);
        if (ret == AVERROR(EAGAIN)) {
            return frames_decoded > 0 ? frames_decoded : ERROR_EOF;
        }
        if (ret == AVERROR_EOF) {
            return frames_decoded > 0 ? frames_decoded : ERROR_EOF;
        }
        if (ret < 0) {
            logf("[decoder] WMA decode error: %d", ret);
            return ERROR_DECODE;
        }

        const uint8_t *in_data[AV_NUM_DATA_POINTERS];
        for (int i = 0; i < impl->frame->channels; i++) {
            in_data[i] = impl->frame->extended_data[i];
        }

        int out_samples = max_frames - frames_decoded;
        float *out_ptr = out + frames_decoded * 2;
        int converted = swr_convert(impl->swr, reinterpret_cast<uint8_t **>(&out_ptr), out_samples,
                                    in_data, impl->frame->nb_samples);
        if (converted < 0) {
            logf("[decoder] Audio resample error: %d", converted);
            return ERROR_DECODE;
        }

        frames_decoded += converted;
        if (converted < out_samples) break;
    }

    return frames_decoded;
}

void audio_close(AudioDecoder *dec)
{
    if (!dec) return;
    if (dec->opaque) {
        audio_cleanup(static_cast<AudioDecoderImpl *>(dec->opaque));
        delete static_cast<AudioDecoderImpl *>(dec->opaque);
    }
    delete dec;
}

VideoDecoder *video_open(const uint8_t *data, uint32_t size)
{
    (void)data; (void)size;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_WMV3);
    if (!codec) {
        codec = avcodec_find_decoder(AV_CODEC_ID_VC1);
    }
    if (!codec) {
        logf("[decoder] XMV video decoder (WMV3/VC1) not found in libavcodec");
        return nullptr;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        logf("[decoder] Failed to allocate XMV codec context");
        return nullptr;
    }

    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        logf("[decoder] Failed to open XMV decoder");
        return nullptr;
    }

    VideoDecoderImpl *impl = new VideoDecoderImpl();
    impl->codec_ctx = ctx;
    impl->frame = av_frame_alloc();
    impl->frame_rgba = av_frame_alloc();
    impl->pkt = av_packet_alloc();

    if (!impl->frame || !impl->frame_rgba || !impl->pkt) {
        video_cleanup(impl);
        delete impl;
        return nullptr;
    }

    VideoDecoder *dec = new VideoDecoder();
    dec->opaque = impl;
    dec->width = ctx->width;
    dec->height = ctx->height;
    dec->fps_num = ctx->framerate.num;
    dec->fps_den = ctx->framerate.den;

    logf("[decoder] XMV video decoder opened: %ux%u @ %u/%u fps", dec->width, dec->height,
         dec->fps_num, dec->fps_den);
    return dec;
}

int video_decode(VideoDecoder *dec, uint8_t *out_rgba)
{
    if (!dec || !dec->opaque || !out_rgba) return ERROR_BUFFER;

    VideoDecoderImpl *impl = video_impl_from_handle(dec);

    if (!impl->sws && impl->codec_ctx) {
        impl->sws = sws_getContext(impl->codec_ctx->width, impl->codec_ctx->height,
                                   impl->codec_ctx->pix_fmt,
                                   impl->codec_ctx->width, impl->codec_ctx->height,
                                   AV_PIX_FMT_RGBA,
                                   SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!impl->sws) {
            logf("[decoder] Failed to initialize video scaler");
            return ERROR_DECODE;
        }

        impl->frame_rgba->format = AV_PIX_FMT_RGBA;
        impl->frame_rgba->width = impl->codec_ctx->width;
        impl->frame_rgba->height = impl->codec_ctx->height;
        if (av_frame_get_buffer(impl->frame_rgba, 32) < 0) {
            logf("[decoder] Failed to allocate RGBA frame buffer");
            return ERROR_DECODE;
        }
    }

    int ret = avcodec_receive_frame(impl->codec_ctx, impl->frame);
    if (ret == AVERROR(EAGAIN)) {
        return 0;
    }
    if (ret == AVERROR_EOF) {
        return 0;
    }
    if (ret < 0) {
        logf("[decoder] XMV decode error: %d", ret);
        return ERROR_DECODE;
    }

    const uint8_t *src_data[4] = {impl->frame->data[0], impl->frame->data[1], impl->frame->data[2], impl->frame->data[3]};
    int src_linesize[4] = {impl->frame->linesize[0], impl->frame->linesize[1], impl->frame->linesize[2], impl->frame->linesize[3]};
    uint8_t *dst_data[4] = {impl->frame_rgba->data[0], impl->frame_rgba->data[1], impl->frame_rgba->data[2], impl->frame_rgba->data[3]};
    int dst_linesize[4] = {impl->frame_rgba->linesize[0], impl->frame_rgba->linesize[1], impl->frame_rgba->linesize[2], impl->frame_rgba->linesize[3]};

    sws_scale(impl->sws, src_data, src_linesize, 0, impl->codec_ctx->height, dst_data, dst_linesize);

    int frame_size = impl->codec_ctx->width * impl->codec_ctx->height * 4;
    memcpy(out_rgba, impl->frame_rgba->data[0], frame_size);

    return 1;
}

void video_close(VideoDecoder *dec)
{
    if (!dec) return;
    if (dec->opaque) {
        video_cleanup(static_cast<VideoDecoderImpl *>(dec->opaque));
        delete static_cast<VideoDecoderImpl *>(dec->opaque);
    }
    delete dec;
}

#else // !HAVE_FFMPEG_HEADERS

// Stub implementations when FFmpeg headers not available
AudioDecoder *audio_open(const uint8_t *, uint32_t)
{
    logf("[decoder] FFmpeg not available: audio decoder disabled");
    return nullptr;
}

int audio_decode(AudioDecoder *, float *, uint32_t)
{
    return ERROR_INIT;
}

void audio_close(AudioDecoder *) {}

VideoDecoder *video_open(const uint8_t *, uint32_t)
{
    logf("[decoder] FFmpeg not available: video decoder disabled");
    return nullptr;
}

int video_decode(VideoDecoder *, uint8_t *)
{
    return ERROR_INIT;
}

void video_close(VideoDecoder *) {}

#endif // HAVE_FFMPEG_HEADERS

} // namespace decoder