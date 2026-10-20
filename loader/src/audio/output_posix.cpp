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
#include <ctime>

namespace audio_out {
namespace {

constexpr uint32_t QUEUED = 6;
constexpr uint32_t RING = QUEUED + 1; // 7 blocks = 70 ms buffer

SDL_AudioDeviceID g_device = 0;
float g_ring[RING][BLOCK * 2];
// Monotonic block counts, wrapped for indexing only. The audio callback is the only reader and the mixer
// thread the only writer, so two counters and a memcpy are all the synchronisation the ring needs. 32 bits
// of 10 ms blocks is more than a year of audio.
std::atomic<uint32_t> g_written{0};
std::atomic<uint32_t> g_read{0};
// The wall-clock side of the mixer. Only the worker thread touches these, which is why they need no atomics.
bool g_clock_started = false;
uint64_t g_clock_start_us = 0;
uint64_t g_clock_blocks = 0;
FILE *g_wav = nullptr;
uint32_t g_wav_frames = 0;

uint64_t now_us()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

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
        const uint32_t r = g_read.load(std::memory_order_relaxed);
        if (r == g_written.load(std::memory_order_acquire)) {
            // Nothing was mixed for this block. Silence is the correct output here, and the read pointer
            // stays put so the next block the mixer does write is not skipped.
            memset(dst + f * 2, 0, (size_t)block_frames * 4);
            continue;
        }
        float *src = g_ring[r % RING];
        for (int i = 0; i < block_frames * 2; i++) {
            float v = src[i];
            dst[f * 2 + i] = static_cast<int16_t>(std::max(-1.0f, std::min(1.0f, v)) * 32767.0f);
        }
        g_read.store(r + 1, std::memory_order_release);
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

    // The callback writes silence for blocks the mixer has not produced, so the ring starts empty rather
    // than pre-filled with stale blocks that would be played as if they were mixed audio.
    for (uint32_t i = 0; i < RING; i++) {
        memset(g_ring[i], 0, sizeof g_ring[i]);
    }
    g_written.store(0, std::memory_order_relaxed);
    g_read.store(0, std::memory_order_relaxed);
    g_clock_started = false;

    SDL_PauseAudioDevice(g_device, 0);
    logf("[audio] SDL2 output: %u Hz stereo, %u ms blocks, %u queued", RATE, BLOCK * 1000 / RATE, QUEUED);
    return true;
}

uint32_t blocks_wanted()
{
    // The mixer's clock is the wall clock, not the sound device. A stream's packet completion is what the
    // game waits on to decode the next one, so a device that is open but not pulling -- a suspended
    // PulseAudio sink, a session with no sound card, a debugger stop -- must not stop the game's audio
    // thread: the intro movie stalled on its first packets for exactly that reason. With a working device
    // the two clocks run at the same rate and the ring only absorbs the difference.
    const uint64_t block_us = (uint64_t)BLOCK * 1000000ull / RATE;
    const uint64_t now = now_us();
    if (!g_clock_started) {
        g_clock_started = true;
        g_clock_start_us = now;
        g_clock_blocks = 0;
    }
    const uint64_t due = (now - g_clock_start_us) / block_us;
    if (due <= g_clock_blocks) return 0;
    uint64_t want = due - g_clock_blocks;
    // A stalled process (a breakpoint, a suspended laptop) has a lot of blocks due at once. Catching up in
    // full would burst a minute of audio into a 70 ms ring, so the backlog is dropped, not replayed.
    g_clock_blocks = due;
    if (want > QUEUED) want = QUEUED;
    return (uint32_t)want;
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

    const uint32_t w = g_written.load(std::memory_order_relaxed);
    memcpy(g_ring[w % RING], lr, sizeof g_ring[0]);
    g_written.store(w + 1, std::memory_order_release);
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