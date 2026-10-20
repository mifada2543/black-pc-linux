// One draw's material: the pipeline it is recorded with, its constants, and its textures.
//
// See src/d3d/draw_material_posix.cpp. The draw path calls this before recording, and does not record when
// it says no -- a draw recorded without a pipeline is what crashed the Intel driver before the guard in
// gfx::draw existed, and a draw recorded with the wrong one is a wrong picture.
#pragma once

#include <stdint.h>

namespace xd3d {

// Bind the material the device state describes. `stream0_stride` is the stride the draw itself carries
// (DrawVerticesUP's argument) or zero when the draw reads a stream whose own stride applies. Returns false
// with a reason; the draw must not be recorded then.
bool bind_draw_material(uint32_t stream0_stride, const char **err);

// Destroy every cached pipeline and uploaded texture. For teardown and tests.
void material_forget_all();

} // namespace xd3d
