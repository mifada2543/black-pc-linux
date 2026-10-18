// Linux audio output using SDL2.
// Port of Windows audio/output.cpp to SDL2 audio device.
#pragma once

#include <cstdint>

namespace audio_out {

constexpr uint32_t RATE = 48000;
constexpr uint32_t BLOCK = 480; // frames per submitted block (10 ms)

// Open the audio device. Returns false if no audio device available.
bool open();

// How many more blocks the device can queue now. Returns 0 if audio not open.
uint32_t blocks_wanted();

// Submit one BLOCK of interleaved L/R float samples (copied).
// If audio not open, this is a no-op.
void submit(const float *lr);

// Simulate device lost (for tests).
void simulate_lost();

} // namespace audio_out