// Turning a table of HLE rows into thunks that are actually callable.
//
// The tables are the Windows port's, ported: an address in the image, the name of the console function
// there, which of our functions implements it, and the shape of the game's call site. What this file adds
// is the part that was implicit before -- a table is a list of strings and function pointers, and nothing
// checked that any of it was usable. Here every row is parsed, built, and counted, and a row that cannot
// be built is reported *by name* rather than skipped: a thunk that silently fails to build means the game
// runs the original console code, which for a function that pokes NV2A registers is a corrupt frame
// rather than a message.
#pragma once

#include "d3d/d3d_hooks_posix.h" // Hle

#include <stddef.h>

#include <string>
#include <vector>

namespace xd3d {

// What building a table produced. The counts are the point: a table with fewer built rows than it has
// entries is a table with holes in it, and the names say where.
struct TableReport {
    size_t rows = 0;
    size_t built = 0;
    struct Refusal {
        std::string name;
        uint32_t address;
        std::string reason;
    };
    std::vector<Refusal> refused;
};

// Build a thunk for every row and hand back where each one landed. `thunks[i]` is the thunk for `rows[i]`,
// or null with a reason in the report. The thunks come from the arena beside the image, so the hook that
// jumps to one always reaches it.
TableReport build_table(const Hle *rows, size_t count, std::vector<void *> *thunks = nullptr);

// The row tables. Kept in one place so a test can check them all without knowing which file each came
// from, and so a build that grows a table cannot quietly lose one.
const Hle *device_rows(size_t *count);
const Hle *resource_rows(size_t *count);
const Hle *draw_rows(size_t *count);

} // namespace xd3d