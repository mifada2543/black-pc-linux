// The HLE thunk builder: turns one table row into machine code that forwards a game's call to our
// implementation.
//
// Why this exists at all: the game's D3D section is LTCG-compiled, so some functions take arguments in
// registers instead of on the stack ("_4__LTCG_eax1" in the function names says which), and the console's
// calling convention is __stdcall for both. A C++ implementation with a normal signature cannot be called
// from such a call site, so each replaced function gets a small generated stub that reshapes the call.
//
// Ported from loader/windows/src/d3d/dispatch.cpp:make_thunk. The mechanism is the same; three things are
// stricter here, each because the invoice's version could fail silently:
//
//   * The parameter spec is validated before anything is emitted, and a bad one is refused *by name*.
//   * "esp" is refused. `push esp` pushes the value of esp *before* the push, so a spec containing it would
//     forward the stack pointer instead of an argument, and the symptom would appear inside the
//     implementation rather than here.
//   * The emitted size is bounded. The invoice builds into a 256-byte buffer and adds per token without
//     checking, so a long spec (a real one has eight "s" already) walks past the end of it. The size is
//     computed first and anything that does not fit is refused.
#pragma once

#include "d3d/d3d_hooks_posix.h" // the Hle row this builds from

#include <stddef.h>

namespace xd3d {

// The contract between a table row and its implementation, which is worth stating because nothing checks
// it at compile time and a mismatch is a crash rather than an error:
//
//   * The implementation is __stdcall and declares exactly as many parameters as the spec has stack "s"
//     tokens -- count, not guesswork. The thunk pushes those arguments and then lets the implementation
//     pop them, so an implementation that declares fewer leaves the rest on the stack, and the thunk's
//     own `pop ecx` / `pop edx` then read arguments and its `ret imm16` returns into one. Pairing an
//     eight-token spec with a four-parameter function crashed this loader's test suite exactly that way.
//   * Register tokens are read from the registers at the moment the call was made, so the game must not
//     expect the implementation to see anything else in them.
//   * "s" means the n-th word of the caller's argument list, counting from the first: on the console the
//     first argument is at the lowest address, which is what the thunk's offsets assume.

// Build the thunk for `h` and return where it was placed, or nullptr with `err` saying why. The code comes
// from the shared arena next to the image (hooks_alloc_thunk), so the hook's jump to it always reaches.
void *make_hle_thunk(const Hle &h, std::string *err = nullptr);

// The parsed form of a spec, exposed because the test checks the parse on its own: which parameters come
// from the stack and in what order, and which from which register.
struct ThunkPlan {
    int stack_args = 0;      // how many words the thunk must drop on return
    bool pushes_eax = false; // !returns: eax is saved too, because a void function may return anything
    bool valid = false;
    char error[96] = {0};
};

ThunkPlan plan_hle_thunk(const char *params, bool returns);

// How many bytes the thunk for `h` will be, or 0 if the spec is bad. Useful for a test that wants to know
// the size without building anything.
size_t hle_thunk_size(const Hle &h);

} // namespace xd3d