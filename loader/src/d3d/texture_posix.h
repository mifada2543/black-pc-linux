// Xbox textures into Vulkan images.
//
// See src/d3d/texture_posix.cpp for what is decoded and why. The two entry points a draw needs are
// texture_image() and texture_sampler(): the pixels for a stage's texture, and the sampler that stage's
// state describes. Both return null with a reason, and a draw that cannot get them is refused rather than
// recorded with a placeholder -- a wrong picture is the one outcome this port does not allow.
#pragma once

#include <stdint.h>

#include "gfx/gfx_vulkan.h"

namespace xd3d {

struct Resource;

// The gfx image for a texture or render-target surface, decoded from Xbox memory into B8G8R8A8. Cached per
// resource and re-decoded at most once per frame, which is what a movie texture written every swap needs.
const gfx::Image *texture_image(Resource *texture, const char **err);

// The sampler for one texture stage, built from that stage's sampler state (address modes, filters, LOD
// bias, anisotropy). Cached until the state changes.
const gfx::Sampler *texture_sampler(uint32_t stage, const char **err);

// Drop the cached image for a resource before its memory goes away.
void texture_forget(Resource *texture);
void texture_forget_all();

// How many resources currently have a cached image. For tests and the run's own accounting.
uint32_t texture_cache_size();

} // namespace xd3d
