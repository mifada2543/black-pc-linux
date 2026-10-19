// The A9 decoder, exercised end to end.
//
// The point of this test is that it does not merely ask whether a decoder opened. It encodes a WMAv2
// packet stream with libavcodec, feeds those packets through this repository's decoder, and checks that
// the samples that come back are the sine waves that went in. An implementation that opens a decoder and
// never calls avcodec_send_packet passes an "opened" check and fails this one.
//
// The XMV half decodes the first frames of a real game video when the file is present. That is the G6
// evidence, not the A9 gate: the gate is the synthetic WMA round trip, because the game's video files are
// large and not part of the repository.
#include "audio/decoder_posix.h"
#include "audio/stream_decode_posix.h"

// FFmpeg's public headers are plain C: wrap them so the test links against the C symbols.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
}

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

static int g_checks = 0, g_failures = 0;

static void check(bool ok, const char *what)
{
    printf("  %s   %s\n", ok ? "ok  " : "FAIL", what);
    g_checks++;
    if (!ok) g_failures++;
}

// ---- WMAv2 encoder, used only to make the test's input ----

struct EncodedAudio {
    std::vector<std::vector<uint8_t>> packets;
    std::vector<uint8_t> extradata;
    uint16_t channels = 0;
    uint16_t block_align = 0;
    uint32_t sample_rate = 0;
    uint32_t bit_rate = 0;
};

static bool fill_sine(AVFrame *frame, int sample_rate, int64_t start, double left_hz, double right_hz)
{
    if (av_frame_make_writable(frame) < 0) return false;

    for (int ch = 0; ch < frame->ch_layout.nb_channels; ch++) {
        const double hz = ch == 0 ? left_hz : right_hz;
        for (int i = 0; i < frame->nb_samples; i++) {
            const double t = static_cast<double>(start + i) / sample_rate;
            const double sample = 0.5 * sin(2.0 * M_PI * hz * t);
            if (frame->format == AV_SAMPLE_FMT_FLTP) {
                reinterpret_cast<float *>(frame->data[ch])[i] = static_cast<float>(sample);
            } else if (frame->format == AV_SAMPLE_FMT_S16P) {
                reinterpret_cast<int16_t *>(frame->data[ch])[i] = static_cast<int16_t>(sample * 32767.0);
            } else if (frame->format == AV_SAMPLE_FMT_FLT) {
                reinterpret_cast<float *>(frame->data[0])[i * frame->ch_layout.nb_channels + ch] =
                    static_cast<float>(sample);
            } else if (frame->format == AV_SAMPLE_FMT_S16) {
                reinterpret_cast<int16_t *>(frame->data[0])[i * frame->ch_layout.nb_channels + ch] =
                    static_cast<int16_t>(sample * 32767.0);
            } else {
                return false;
            }
        }
    }
    return true;
}

static bool encode_wma(EncodedAudio &out)
{
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_WMAV2);
    if (!codec) return false;

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) return false;

    const int sample_rate = 44100;
    ctx->sample_rate = sample_rate;
    ctx->bit_rate = 128000;
    ctx->time_base = AVRational{1, sample_rate};
    av_channel_layout_default(&ctx->ch_layout, 2);
    ctx->sample_fmt = codec->sample_fmts ? codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;

    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        printf("  (the local libavcodec cannot open a WMAv2 encoder: %s)\n", codec->name);
        avcodec_free_context(&ctx);
        return false;
    }

    const int frame_size = ctx->frame_size > 0 ? ctx->frame_size : 2048;
    AVFrame *frame = av_frame_alloc();
    frame->format = ctx->sample_fmt;
    frame->sample_rate = ctx->sample_rate;
    frame->nb_samples = frame_size;
    av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout);
    if (av_frame_get_buffer(frame, 0) < 0) {
        av_frame_free(&frame);
        avcodec_free_context(&ctx);
        return false;
    }

    const int total_frames = sample_rate / 2; // half a second
    int64_t produced = 0;
    AVPacket *pkt = av_packet_alloc();

    auto drain = [&]() {
        while (avcodec_receive_packet(ctx, pkt) == 0) {
            out.packets.emplace_back(pkt->data, pkt->data + pkt->size);
            av_packet_unref(pkt);
        }
    };

    for (int64_t pos = 0; pos < total_frames; pos += frame_size) {
        frame->nb_samples = static_cast<int>(std::min<int64_t>(frame_size, total_frames - pos));
        if (!fill_sine(frame, sample_rate, pos, 440.0, 660.0)) break;
        frame->pts = pos;
        if (avcodec_send_frame(ctx, frame) < 0) break;
        drain();
    }
    avcodec_send_frame(ctx, nullptr);
    drain();

    out.channels = static_cast<uint16_t>(ctx->ch_layout.nb_channels);
    out.block_align = static_cast<uint16_t>(ctx->block_align);
    out.sample_rate = static_cast<uint32_t>(ctx->sample_rate);
    out.bit_rate = static_cast<uint32_t>(ctx->bit_rate);
    if (ctx->extradata && ctx->extradata_size > 0) {
        out.extradata.assign(ctx->extradata, ctx->extradata + ctx->extradata_size);
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return !out.packets.empty();
}

// Zero crossings are enough to tell 440 Hz from 660 Hz after a lossy codec; a full FFT would be more
// precise and would test the FFT rather than the decoder.
static int count_zero_crossings(const std::vector<float> &samples, size_t start, size_t frames, int stride,
                                int channel)
{
    int crossings = 0;
    float previous = 0;
    for (size_t i = 0; i < frames; i++) {
        const float value = samples[(start + i) * stride + channel];
        if ((previous < 0 && value >= 0) || (previous >= 0 && value < 0)) crossings++;
        previous = value;
    }
    return crossings;
}

static void test_wma_round_trip()
{
    printf("\nWMAv2 decode round trip\n");

    EncodedAudio encoded;
    if (!encode_wma(encoded)) {
        check(false, "the test can encode a WMAv2 packet stream");
        return;
    }
    check(!encoded.packets.empty(), "the encoder produced packets");
    check(encoded.channels == 2, "the encoder produced stereo");
    check(encoded.sample_rate == 44100, "the encoder produced 44.1 kHz");

    decoder::AudioFormat fmt;
    fmt.format_tag = 0x0161;
    fmt.channels = encoded.channels;
    fmt.sample_rate = encoded.sample_rate;
    fmt.avg_bytes_per_sec = encoded.bit_rate / 8;
    fmt.block_align = encoded.block_align;
    fmt.bits_per_sample = 16;
    fmt.extradata = encoded.extradata.data();
    fmt.extradata_size = static_cast<uint32_t>(encoded.extradata.size());

    decoder::AudioDecoder *dec = decoder::audio_open(fmt);
    check(dec != nullptr, "the decoder opens the stream's format");
    if (!dec) return;

    check(dec->sample_rate == 48000, "the decoder resamples to the host mixer's 48 kHz");
    check(dec->channels == 2, "the decoder produces stereo");

    std::vector<float> decoded;
    float block[480 * 2];
    uint32_t packets_used = 0;
    uint32_t empty_calls = 0;
    bool flushed = false;

    // Push packets and drain as the mixer would. The decoder queues packets, so this loop may hand over
    // the next packet while the previous packet's frames are still coming out. Once the last packet is
    // in, stream_flush() marks the end; from then on the calls only drain.
    for (;;) {
        const uint8_t *data = nullptr;
        uint32_t size = 0;
        if (packets_used < encoded.packets.size()) {
            data = encoded.packets[packets_used].data();
            size = static_cast<uint32_t>(encoded.packets[packets_used].size());
            packets_used++;
        } else if (!flushed) {
            decoder::audio_flush(dec);
            flushed = true;
        }

        const int frames = decoder::audio_decode(dec, data, size, block, 480);
        if (frames == decoder::ERROR_EOF) break;
        if (frames < 0) {
            printf("  (decode returned %d)\n", frames);
            break;
        }
        if (frames > 0) {
            decoded.insert(decoded.end(), block, block + frames * 2);
        } else if (flushed && ++empty_calls > 8) {
            break; // safety net: the stream is flushed and the decoder still says nothing
        }
    }

    decoder::audio_close(dec);

    const size_t frames = decoded.size() / 2;
    printf("  (decoded %u packets into %zu frames of 48 kHz stereo)\n", packets_used, frames);
    check(frames > 12000, "the decoder produced about half a second of audio");
    if (frames < 6000) return;

    // Skip the codec's priming delay, then measure the middle of the stream.
    const size_t start = std::min<size_t>(4800, frames / 4);
    const size_t window = std::min<size_t>(4800, frames - start);

    double energy = 0;
    for (size_t i = 0; i < window; i++) {
        const float l = decoded[(start + i) * 2];
        const float r = decoded[(start + i) * 2 + 1];
        energy += static_cast<double>(l) * l + static_cast<double>(r) * r;
    }
    const double rms = sqrt(energy / (window * 2));
    check(rms > 0.15 && rms < 0.55, "the decoded signal has the amplitude that went in");

    // 440 Hz over 100 ms is 44 cycles, 660 Hz is 66; each cycle crosses zero twice.
    const int left = count_zero_crossings(decoded, start, window, 2, 0);
    const int right = count_zero_crossings(decoded, start, window, 2, 1);
    const int left_periods = left / 2;
    const int right_periods = right / 2;
    check(left_periods >= 38 && left_periods <= 50, "the left channel is 440 Hz");
    check(right_periods >= 58 && right_periods <= 74, "the right channel is 660 Hz");

    // The two channels must not be the same signal: that would mean one channel was copied twice.
    double difference = 0;
    for (size_t i = 0; i < window; i++) {
        const double d = decoded[(start + i) * 2] - decoded[(start + i) * 2 + 1];
        difference += d * d;
    }
    check(sqrt(difference / window) > 0.05, "the two channels are decoded independently");
}

static void test_refusals()
{
    printf("\ndecoder refusals\n");

    decoder::AudioFormat unknown;
    unknown.format_tag = 0x1234;
    check(decoder::audio_open(unknown) == nullptr, "an unknown audio format tag is refused");

    check(decoder::video_open(nullptr, 0) == nullptr, "video_open refuses a null buffer");
    check(decoder::video_open_file("/nonexistent/xmv-that-is-not-there") == nullptr,
          "video_open_file refuses a missing file");
    check(decoder::audio_decode(nullptr, nullptr, 0, nullptr, 0) == decoder::ERROR_BUFFER,
          "audio_decode checks its arguments");
    check(decoder::video_decode(nullptr, nullptr) == decoder::ERROR_BUFFER,
          "video_decode checks its arguments");
}

static std::vector<uint8_t> read_file(const std::string &path)
{
    std::vector<uint8_t> data;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return data;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 0) {
        data.resize(static_cast<size_t>(size));
        if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
    }
    fclose(f);
    return data;
}

static void test_xmv(const std::string &path)
{
    printf("\nXMV video decode\n");

    const std::vector<uint8_t> file = read_file(path);
    if (file.empty()) {
        printf("  skip: %s is not present (the repository does not ship game data)\n", path.c_str());
        return;
    }

    decoder::VideoDecoder *dec = decoder::video_open(file.data(), static_cast<uint32_t>(file.size()));
    check(dec != nullptr, "the decoder opens a real XMV file from memory");
    if (!dec) return;

    check(dec->width == 640 && dec->height == 480, "the video is 640x480");
    check(dec->fps_num > 0 && dec->fps_den > 0, "the frame rate came through");

    std::vector<uint8_t> frame(static_cast<size_t>(dec->width) * dec->height * 4);

    // The first frames of a cutscene can legitimately be black (fades), so look at a few before calling
    // the output flat. Any one frame with several distinct pixels proves the pipeline produced an image.
    size_t best_distinct = 0;
    int decoded_frames = 0;
    for (int i = 0; i < 10; i++) {
        const int ret = decoder::video_decode(dec, frame.data());
        if (ret == 1) {
            decoded_frames++;
            uint32_t seen[64] = {};
            size_t distinct = 0;
            for (size_t p = 0; p + 3 < frame.size(); p += 4 * 37) {
                const uint32_t pixel = frame[p] | (frame[p + 1] << 8) | (frame[p + 2] << 16);
                bool known = false;
                for (size_t k = 0; k < distinct; k++)
                    if (seen[k] == pixel) known = true;
                if (!known && distinct < 64) seen[distinct++] = pixel;
            }
            best_distinct = distinct > best_distinct ? distinct : best_distinct;
        } else if (ret == decoder::ERROR_EOF) {
            break;
        } else {
            break;
        }
    }

    check(decoded_frames >= 1, "the first frame decodes");
    check(best_distinct > 4, "the frames are images, not one flat colour");

    decoder::video_close(dec);

    decoder::VideoDecoder *from_disk = decoder::video_open_file(path.c_str());
    check(from_disk != nullptr, "the same file opens through the path API");
    if (from_disk) {
        std::vector<uint8_t> disk_frame(static_cast<size_t>(from_disk->width) * from_disk->height * 4);
        check(decoder::video_decode(from_disk, disk_frame.data()) == 1,
              "and its first frame decodes as well");
        decoder::video_close(from_disk);
    }
}

int main()
{
    printf("BLACK loader -- A9: the audio/video decoder\n");

    test_wma_round_trip();
    test_refusals();
#ifdef BLACKLINUX_SOURCE_DIR
    test_xmv(std::string(BLACKLINUX_SOURCE_DIR) + "/game/videos/02_n.xmv");
#endif

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
