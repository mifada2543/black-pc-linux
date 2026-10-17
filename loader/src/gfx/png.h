// A PNG writer, small enough to read in one sitting.
//
// Why not a library: the loader's only use is to save a frame for a bug report or a screenshot, and that
// has to work on a machine with nothing installed but the driver. So this writes uncompressed deflate
// blocks ("stored" blocks) and computes the two checksums by hand. The result is a valid PNG that any
// viewer and any image library opens, at the cost of a slightly larger file -- which for a debugging
// artefact nobody keeps is the right trade.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace gfx {

// `rgba` is width*height*4 bytes, row-major from the top. False on a bad argument or a write error.
bool write_png(const char *path, const void *rgba, uint32_t width, uint32_t height);

} // namespace gfx
