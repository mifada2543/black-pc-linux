// Kernel import resolution for the Linux port: ordinal -> implementation, or the logging stub.
//
// The data in ktable_posix.cpp is the Windows reference table (src/kernel/ktable.cpp) transcribed, and
// the thunk generation follows the same design. This header is the surface the boot path uses.
#pragma once

#include <stdint.h>

#include <string>

class XbeImage;
class XbePosix;

// Fill the mapped image's kernel import table: every slot becomes the address of an implementation, a
// data cell, or a generated logging stub. Ordinals 302 and 312 are bound to this port's own
// seh_raise / seh_unwind so the game and the loader share one exception chain.
// Returns false with a reason for an unknown ordinal or an unreadable table.
bool kernel_resolve_thunks(const XbePosix &x, XbeImage &img, std::string &err);

const char *kernel_export_name(uint32_t ordinal);

// What the last resolve did, for the boot log and the tests.
struct KernelThunkStats {
    int imports = 0;      // slots filled before the zero terminator
    int implemented = 0;  // bound to a real function
    int data_exports = 0; // bound to a cell holding a real address
    int stubbed = 0;      // bound to the logging stub
    std::string stub_names;
};
const KernelThunkStats &kernel_thunk_stats();

// How many times the stub for an ordinal has been called -- the boot log's first-call list comes from
// this, and it is also how the tests know game code reached the stub.
uint32_t kernel_stub_calls(uint32_t ordinal);

// Log every stub call, not just the first.
void kernel_set_trace(bool on);

// Make an unimplemented kernel call end the run instead of being logged. The Windows port has this
// (ktable.cpp asks request_exit(2, ...) from the stub tail) and --strict is advertised in the usage text;
// on this port the flag used to be parsed and ignored, so an unimplemented call was only ever a log line.
void kernel_set_strict(bool on);

// Declare an ordinal unimplemented, whatever the HLE modules bind, and put it back afterwards. Only the
// --strict path needs this: it can be tested by reaching a stub from game code, and there are no stubs left.
void kernel_hold_unimplemented(uint32_t ordinal);
void kernel_release_held();

// For the HLE modules that land later: give an ordinal a real target, or point a data export's cell at
// an address. kernel_resolve_thunks calls kernel_bind_implemented() first, which is where a new module
// registers its ordinals; kernel_bind_export is for anything that needs to happen later than that.
bool kernel_bind_export(uint32_t ordinal, void *target);
void kernel_bind_implemented();
bool kernel_bind_variable(uint32_t ordinal, void *address);