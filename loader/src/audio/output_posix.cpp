// Linux audio output implementation using SDL2.
// Port of Windows audio/output.cpp to SDL2 audio device.
#include "audio/output_posix.h"

#include "platform/posix/log.h"

#include <SDL2/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace audio_out {
namespace {

constexpr uint32_t QUEUED = 6;
constexpr uint32_t RING = QUEUED + 1; // 7 blocks = 70 ms buffer

SDL_AudioDeviceID g_device = 0;
float g_ring[RING][BLOCK * 2];
uint32_t g_next = 0;
uint32_t g_submitted = 0;
FILE *g_wav = nullptr;
uint32_t g_wav_frames = 0;

void wav_header()
{
    if (!g_wav) return;
    uint32_t data = g_wav_frames * 4; // 16-bit stereo = 4 bytes/frame
    struct {
        char riff[4]; uint32_t size; char wave[4], fmt[4]; uint32_t fmt_size;
        uint16_t tag, ch; uint32_t rate, bytes_per_s; uint16_t align, bits;
        char data[4]; uint32_t data_size;
    } h = {{'R', 'I', 'F', 'F'}, 36 + data, {'W', 'A', 'V', 'E'}, {'f', 'm', 't', ' '}, 16,
           1, 2, RATE, RATE * 4, 4, 16, {'d', 'a', 't', 'a'}, data};
    fseek(g_wav, 0, SEEK_SET);
    fwrite(&h, sizeof h, 1, g_wav);
    fseek(g_wav, 0, SEEK_END);
}

void sdl_audio_callback(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    int frames = len / 4; // 2 channels * 2 bytes = 4 bytes/frame
    int16_t *dst = reinterpret_cast<int16_t *>(stream);

    for (int f = 0; f < frames; f += BLOCK) {
        int block_frames = std::min(frames - f, static_cast<int>(BLOCK));
        float *src = g_ring[g_submitted % RING];
        for (int i = 0; i < block_frames * 2; i++) {
            float v = src[i];
            dst[f * 2 + i] = static_cast<int16_t>(std::max(-1.0f, std::min(1.0f, v)) * 32767.0f);
        }
        g_submitted++;
    }
    // Fill remaining with silence if we underrun
    for (int f = (frames / BLOCK) * BLOCK; f < frames; f++) {
        dst[f * 2] = 0;
        dst[f * 2 + 1] = 0;
    }
}

} // namespace

bool open()
{
    // WAV recording if requested via environment
    const char *wav_path = std::getenv("BLACKLINUX_AUDIO_WAV");
    if (wav_path && wav_path[0]) {
        g_wav = fopen(wav_path, "wb");
        if (g_wav) {
            wav_header();
            logf("[audio] recording the mix to WAV: %s", wav_path);
        }
    }

    SDL_AudioSpec want = {}, have = {};
    want.freq = RATE;
    want.format = AUDIO_S16SYS; // SDL will handle endianness
    want.channels = 2;
    want.samples = BLOCK; // 10 ms buffer
    want.callback = sdl_audio_callback;
    want.userdata = nullptr;

    // The window opens SDL with the video subsystem, which is all it needs; opening the audio device needs
    // the audio subsystem as well. Asking for it here rather than in window_open() keeps the dependency with
    // the code that has it: without this, SDL_OpenAudioDevice fails with "Audio subsystem is not initialized"
    // and the game runs silently.
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        logf("[audio] SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
        if (g_wav) { fclose(g_wav); g_wav = nullptr; }
        return false;
    }

    g_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (g_device == 0) {
        logf("[audio] SDL_OpenAudioDevice failed: %s", SDL_GetError());
        if (g_wav) { fclose(g_wav); g_wav = nullptr; }
        return false;
    }

    if (have.freq != RATE || have.channels != 2) {
        logf("[audio] got %u Hz %d ch (wanted %u Hz stereo)", have.freq, have.channels, RATE);
    }

    // Pre-fill ring buffer with silence
    for (uint32_t i = 0; i < RING; i++) {
        memset(g_ring[i], 0, sizeof g_ring[i]);
    }
    g_next = 0;
    g_submitted = 0;

    SDL_PauseAudioDevice(g_device, 0);
    logf("[audio] SDL2 output: %u Hz stereo, %u ms blocks, %u queued", RATE, BLOCK * 1000 / RATE, QUEUED);
    return true;
}

uint32_t blocks_wanted()
{
    if (g_device == 0) return 0;

    // Estimate how many blocks the device can accept
    // SDL doesn't expose queued buffer count directly, so we estimate from the callback rate
    // A simple heuristic: if we've submitted less than RING blocks, we can submit more
    uint32_t pending = g_next > g_submitted ? g_next - g_submitted : g_next + RING - g_submitted;
    if (pending < QUEUED) {
        return QUEUED - pending;
    }
    return 0;
}

void submit(const float *lr)
{
    if (g_wav) {
        int16_t pcm[BLOCK * 2];
        for (uint32_t i = 0; i < BLOCK * 2; i++) {
            pcm[i] = static_cast<int16_t>(std::max(-1.0f, std::min(1.0f, lr[i])) * 32767.0f);
        }
        fwrite(pcm, sizeof pcm, 1, g_wav);
        g_wav_frames += BLOCK;
        if (g_wav_frames % (RATE / 2) == 0) wav_header();
    }

    if (g_device == 0) return;

    // Copy to ring buffer
    memcpy(g_ring[g_next % RING], lr, sizeof g_ring[0]);
    g_next = (g_next + 1) % RING;
}

void simulate_lost()
{
    logf("[audio] tests: simulating device lost");
    if (g_device) {
        SDL_CloseAudioDevice(g_device);
        g_device = 0;
    }
}

} // namespace audio_out