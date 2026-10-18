// The HLE thunk builder, measured from both ends.
//
// A thunk takes a call the game made -- possibly with arguments in registers, because the game's D3D
// section is LTCG-compiled -- and forwards it to a __stdcall implementation. Three things can go wrong,
// and all three produce a stack that is subtly wrong rather than an obvious crash:
//
//   * the wrong value arrives (an argument read from the wrong offset, or a register not preserved),
//   * the wrong number of bytes is dropped on return, so the game's own stack frame drifts,
//   * the byte count happens to be right for the tests here and wrong for the real caller.
//
// So the suite calls *through* built thunks with a hand-written caller that records esp before and after,
// and eax/ecx/edx afterwards, and compares. The last four stdcall-shaped bugs in this port were all of
// this shape -- the SEH record order, ordinal 302's calling convention, the DPC's `ret 16`, and the 64-bit
// argument occupying two stack slots -- which is why this suite exists at all.
//
// Two facts about writing the caller by hand, both learned the hard way and both written down here so the
// next person does not rediscover them:
//
//   * The argument list is kept to three words and the data goes through one struct pointer. Five separate
//     pointer arguments sounded clearer, but the compiler reshapes that call (it inlines the bridge and
//     folds the arguments together), and what the hand-written code then reads is not what the source
//     says. One struct pointer has fixed offsets: stack at +0, regs at +32, out at +44.
//   * The measurement slots live at -28(%ebp) and below. -4/-8/-12 hold the saved ebx/esi/edi, and a
//     measurement that overwrites them hands the caller a corrupted register -- which shows up much later,
//     as a wrong value in an unrelated global.
//
// And one about this toolchain, because it cost an afternoon: its assembler rejects `mov` with a memory
// source *and* a memory destination ("operand size mismatch"), which is not i386's rule -- opcode 0x89 takes
// two memory operands. Every copy below therefore goes through a register. If a future toolchain accepts
// the form, the copies can be shortened; nothing depends on them being absent.
#include "d3d/thunk_posix.h"

#include <stdio.h>
#include <string.h>

#include <string>

static int g_failures, g_checks;

static void check(bool cond, const std::string &what)
{
    g_checks++;
    if (cond) {
        printf("  ok   %s\n", what.c_str());
    } else {
        printf("  FAIL %s\n", what.c_str());
        g_failures++;
    }
    fflush(stdout);
}

static void check_eq(uint64_t got, uint64_t want, const std::string &what)
{
    char buf[200];
    snprintf(buf, sizeof buf, "%s (got %llu, want %llu)", what.c_str(), (unsigned long long)got,
             (unsigned long long)want);
    check(got == want, buf);
}

// ---- what the caller sets up, and what the implementation saw ---------------------------
//
// Everything goes through the caller's frame rather than through globals: this is a PIE, and hand-written
// assembly that names a global needs an absolute relocation, which the linker refuses (or, with
// -z notext, silently turns into a text relocation). Passing the data in and out as arguments is both
// linkable and clearer about what the measurement is.

enum {
    IO_STACK = 0,   // uint32_t[8]: the arguments the caller pushes, first one lowest
    IO_REGS = 32,   // uint32_t[3]: eax, ecx, edx for the caller to load
    IO_OUT = 44,    // uint32_t[5]: esp before, esp after, eax after, ecx, edx
};

struct CallIo {
    uint32_t stack[8];
    uint32_t regs[3];
    uint32_t out[5];
};

extern "C" void call_thunk(void *thunk, int count, CallIo *io);
__asm__(".text\n"
        ".globl call_thunk\n"
        ".type call_thunk, @function\n"
        "call_thunk:\n"
        "    pushl %ebp\n"
        "    movl %esp, %ebp\n"
        "    pushl %ebx\n"
        "    pushl %esi\n"
        "    pushl %edi\n"
        "    movl 8(%ebp), %ebx\n"             // the thunk to call
        "    movl 16(%ebp), %edi\n"            // the data
        "    movl %esp, %esi\n"               // the stack as it was before the arguments went on it: that
        "    movl %esi, 44(%edi)\n"            // is where esp must be once the thunk has dropped them
        "    movl 12(%ebp), %edx\n"            // how many arguments there are
        "    testl %edx, %edx\n"
        "    je 2f\n"
        "    shll $2, %edx\n"                   // room for them, carved in one go
        "    subl %edx, %esp\n"
        "    movl %esp, %ecx\n"                // first argument ends up lowest, as __stdcall wants
        "    xorl %eax, %eax\n"
        "1:\n"
        "    movl (%edi,%eax,4), %esi\n"       // stack[i]
        "    movl %esi, (%ecx,%eax,4)\n"
        "    incl %eax\n"
        "    cmpl 12(%ebp), %eax\n"
        "    jb 1b\n"
        "2:\n"
        "    movl 32(%edi), %eax\n"            // the registers the caller's code left in place
        "    movl 36(%edi), %ecx\n"
        "    movl 40(%edi), %edx\n"
        "    call *%ebx\n"
        "    movl %esp, %esi\n"
        "    movl %esi, 48(%edi)\n"            // and after it: equal only if the thunk cleaned up exactly
        "    movl %eax, %esi\n"
        "    movl %esi, 52(%edi)\n"            // what the implementation left behind
        "    movl %ecx, %esi\n"
        "    movl %esi, 56(%edi)\n"
        "    movl %edx, %esi\n"
        "    movl %esi, 60(%edi)\n"
        "    leal -12(%ebp), %esp\n"           // back to the three saved registers, wherever the call left esp
        "    popl %edi\n"
        "    popl %esi\n"
        "    popl %ebx\n"
        "    popl %ebp\n"
        "    ret\n"
        ".size call_thunk, .-call_thunk\n");

// ---- what the implementations saw ------------------------------------------------------

static int g_argc_seen = -1;
static uint32_t g_args_seen[8];
static int g_calls_seen = 0;

// ---- the C++ side ------------------------------------------------------------------------

// The bridge to the assembly. noinline on purpose: inlined, the compiler rewrites the argument list and
// the hand-written code no longer reads what this source says. Everything this suite measures depends on
// that call being exactly the cdecl call it looks like.
__attribute__((noinline)) static void do_call(void *thunk, int stack_count, CallIo *io)
{
    call_thunk(thunk, stack_count, io);
}

static CallIo g_io;

static void reset_call()
{
    g_argc_seen = -1;
    g_calls_seen = 0;
    memset(g_args_seen, 0, sizeof g_args_seen);
    for (int i = 0; i < 8; i++) g_io.stack[i] = 0x11111111u * (uint32_t)(i + 1);
    g_io.regs[0] = 0xAAAAAAAAu;
    g_io.regs[1] = 0xBBBBBBBBu;
    g_io.regs[2] = 0xCCCCCCCCu;
    memset(g_io.out, 0, sizeof g_io.out);
}

// The implementations the thunks call. All __stdcall, because that is what the thunk assumes: it lets them
// pop their own parameters and only drops the game's originals on the way out.
extern "C" __attribute__((stdcall)) uint32_t impl_0(void)
{
    g_argc_seen = 0;
    g_calls_seen++;
    return 0x00C0FFEE;
}

extern "C" __attribute__((stdcall)) uint32_t impl_1(uint32_t a)
{
    g_argc_seen = 1;
    g_args_seen[0] = a;
    g_calls_seen++;
    return a + 1;
}

extern "C" __attribute__((stdcall)) uint32_t impl_2(uint32_t a, uint32_t b)
{
    g_argc_seen = 2;
    g_args_seen[0] = a;
    g_args_seen[1] = b;
    g_calls_seen++;
    return a * 2u + b;
}

extern "C" __attribute__((stdcall)) uint32_t impl_3(uint32_t a, uint32_t b, uint32_t c)
{
    g_argc_seen = 3;
    g_args_seen[0] = a;
    g_args_seen[1] = b;
    g_args_seen[2] = c;
    g_calls_seen++;
    return a + b + c;
}

extern "C" __attribute__((stdcall)) uint32_t impl_4(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    g_argc_seen = 4;
    g_args_seen[0] = a;
    g_args_seen[1] = b;
    g_args_seen[2] = c;
    g_args_seen[3] = d;
    g_calls_seen++;
    return a ^ b ^ c ^ d;
}

// Eight parameters: the longest spec the tables actually use. The implementation must declare exactly as
// many parameters as the spec has stack tokens, because the thunk relies on the implementation popping
// them -- pairing an eight-token spec with a four-parameter function is how this suite first crashed.
extern "C" __attribute__((stdcall)) uint32_t impl_8(uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                                                   uint32_t e, uint32_t f, uint32_t g, uint32_t h)
{
    g_argc_seen = 8;
    g_args_seen[0] = a;
    g_args_seen[1] = b;
    g_args_seen[2] = c;
    g_args_seen[3] = d;
    g_args_seen[4] = e;
    g_args_seen[5] = f;
    g_args_seen[6] = g;
    g_args_seen[7] = h;
    g_calls_seen++;
    return a + b + c + d + e + f + g + h;
}

extern "C" __attribute__((stdcall)) void impl_void_2(uint32_t a, uint32_t b)
{
    g_argc_seen = 2;
    g_args_seen[0] = a;
    g_args_seen[1] = b;
    g_calls_seen++;
    // Leaves eax alone, which is the interesting case: the caller had a value there and wants it back.
}

static xd3d::Hle row(uint32_t addr, const char *name, void *impl, const char *params, bool returns)
{
    xd3d::Hle h{};
    h.addr = addr;
    h.name = name;
    h.impl = impl;
    h.params = params;
    h.returns = returns;
    return h;
}

// ---- the checks -------------------------------------------------------------------------

static void test_spec_parsing()
{
    printf("\nreading the parameter spec\n");

    // The spec is what the table says about a game's call site: "s" for a stack argument, a register name
    // otherwise. Both spacing styles appear in the tables, so commas count as separators too.
    struct Case {
        const char *spec;
        int stack_args;
        bool valid;
    };
    const Case cases[] = {
        {"s", 1, true},          {"s s", 2, true},   {"s s s s s s s s", 8, true},
        {"s,s,s", 3, true},      {"eax", 0, true},  {"ecx edx", 0, true},
        {"s eax", 1, true},      {"", 0, true},     {"s", 1, true},
        {"s v", 0, false},       {"eaxx", 0, false}, {"esp", 0, false},
        {"s esp", 0, false},     {"s s s s s s s s s s s s s s s s s", 0, false}, // more than 16
    };
    for (const Case &c : cases) {
        const xd3d::ThunkPlan plan = xd3d::plan_hle_thunk(c.spec, true);
        char buf[160];
        snprintf(buf, sizeof buf, "spec \"%s\" is %s with %d stack argument(s)", c.spec,
                 c.valid ? "valid" : "refused", c.stack_args);
        if (c.valid) {
            check(plan.valid && plan.stack_args == c.stack_args, buf);
        } else {
            check(!plan.valid && plan.error[0] != 0, buf);
        }
    }

    // A void implementation cannot leave anything in eax, so the thunk saves it as well.
    check(xd3d::plan_hle_thunk("s", false).pushes_eax, "a void function saves eax too");
    check(!xd3d::plan_hle_thunk("s", true).pushes_eax, "one that returns a value does not");

    // The refusals name the offending token: "unknown parameter 'v'" is actionable, a null pointer is not.
    const xd3d::ThunkPlan bad = xd3d::plan_hle_thunk("s bogus", true);
    check(strstr(bad.error, "bogus") != nullptr, std::string("the refusal names the token: ") + bad.error);
    const xd3d::ThunkPlan esp = xd3d::plan_hle_thunk("esp", true);
    check(strstr(esp.error, "esp") != nullptr,
          std::string("and push esp is refused with its reason: ") + esp.error);
}

static void test_forwarding()
{
    printf("\ncalling through a thunk, from a hand-written caller\n");

    // The arena is mapped against this "image" address range; nothing is patched, only built into.
    std::string err;
    check(xd3d::hooks_prepare_arena(0x1000, (uint8_t *)(uintptr_t)0x00200000, err),
          "the thunk arena is mapped" + (err.empty() ? std::string() : ": " + err));

    // One stack argument: the simplest shape there is, and the one that proves the offsets.
    {
        reset_call();
        g_io.stack[0] = 0x11223344u;
        g_io.regs[0] = 0xAAAAAAAAu;
        void *t = xd3d::make_hle_thunk(row(0, "one_s", (void *)impl_1, "s", true), &err);
        check(t != nullptr, std::string("the thunk for \"s\" is built") + (t ? "" : ": " + err));
        g_calls_seen = 0;
        do_call(t, 1, &g_io);
        check_eq(g_argc_seen, 1, "the implementation saw one argument");
        check_eq(g_args_seen[0], 0x11223344u, "and it was the one the caller pushed");
        check_eq(g_io.out[2], 0x11223345u, "the return value came back");
        check_eq(g_io.out[1], g_io.out[0], "and the caller's stack is exactly where it was");
        check_eq(g_io.out[3], 0xBBBBBBBBu, "ecx survived the call");
        check_eq(g_io.out[4], 0xCCCCCCCCu, "edx survived it too");
    }

    // Three stack arguments: the offsets have to walk up the frame correctly.
    {
        reset_call();
        g_io.stack[0] = 0x0A0B0C0Du;
        g_io.stack[1] = 0x1A1B1C1Du;
        g_io.stack[2] = 0x2A2B2C2Du;
        void *t = xd3d::make_hle_thunk(row(0, "three_s", (void *)impl_3, "s s s", true), &err);
        check(t != nullptr, "the thunk for \"s s s\" is built");
        do_call(t, 3, &g_io);
        check_eq(g_argc_seen, 3, "the implementation saw three arguments");
        check_eq(g_args_seen[0], g_io.stack[0], "first");
        check_eq(g_args_seen[1], g_io.stack[1], "second");
        check_eq(g_args_seen[2], g_io.stack[2], "third");
        check_eq(g_io.out[2], g_io.stack[0] + g_io.stack[1] + g_io.stack[2], "and the sum came back");
        check_eq(g_io.out[1], g_io.out[0], "the stack is back where it was");
    }

    // Eight stack arguments: the longest real spec. This is the one where a single wrong byte shifts
    // everything, and where the invoice's fixed 256-byte buffer was closest to trouble.
    {
        reset_call();
        void *t = xd3d::make_hle_thunk(row(0, "eight_s", (void *)impl_8, "s s s s s s s s", true), &err);
        check(t != nullptr, "the thunk for eight stack arguments is built");
        check_eq(xd3d::hle_thunk_size(row(0, "x", (void *)impl_8, "s s s s s s s s", true)),
                 2 + 8 * 4 + 5 + 2 + 2 + 3, "and its size is the counted one (pushes, mov, call, pops, ret imm16)");
        do_call(t, 8, &g_io);
        check_eq(g_argc_seen, 8, "the implementation saw all eight arguments");
        check_eq(g_args_seen[0], g_io.stack[0], "first of them is the caller's first");
        check_eq(g_args_seen[7], g_io.stack[7], "and the eighth is the caller's eighth");
        check_eq(g_io.out[2], g_io.stack[0] + g_io.stack[1] + g_io.stack[2] + g_io.stack[3] + g_io.stack[4] +
                                        g_io.stack[5] + g_io.stack[6] + g_io.stack[7],
                 "and the sum of all eight came back");
        check_eq(g_io.out[1], g_io.out[0], "eight words were dropped, so the stack is balanced");
    }

    // Register arguments: the LTCG shape. The game passes them in registers, the implementation declares
    // normal parameters, and the thunk has to bridge the two conventions.
    {
        reset_call();
        g_io.regs[0] = 0x00000011u;
        g_io.regs[1] = 0x00000022u;
        g_io.regs[2] = 0x00000033u;
        void *t = xd3d::make_hle_thunk(row(0, "reg2", (void *)impl_2, "ecx edx", true), &err);
        check(t != nullptr, "the thunk for \"ecx edx\" is built");
        do_call(t, 0, &g_io);
        check_eq(g_argc_seen, 2, "the implementation saw two arguments");
        check_eq(g_args_seen[0], 0x22, "the first came from ecx");
        check_eq(g_args_seen[1], 0x33, "the second from edx");
        check_eq(g_io.out[2], 0x22 * 2u + 0x33, "and the return value came back through eax");
        check_eq(g_io.out[1], g_io.out[0], "nothing was pushed on the stack, so nothing was dropped");
        check_eq(g_io.out[3], 0x22, "ecx still holds what the caller put there");
        check_eq(g_io.out[4], 0x33, "and so does edx");
    }

    // A single register argument, from eax, with nothing on the stack.
    {
        reset_call();
        g_io.regs[0] = 0x0000BEEFu;
        void *t = xd3d::make_hle_thunk(row(0, "reg1", (void *)impl_1, "eax", true), &err);
        check(t != nullptr, "the thunk for \"eax\" is built");
        do_call(t, 0, &g_io);
        check_eq(g_args_seen[0], 0xBEEF, "the argument arrived from eax");
        check_eq(g_io.out[2], 0xBEEF + 1, "and the result came back in eax");
    }

    // Mixed: one stack argument and one register argument, which is where an off-by-one in the offset
    // arithmetic would show up as the wrong register value.
    {
        reset_call();
        g_io.stack[0] = 0x0BADF00Du;
        g_io.regs[1] = 0x76543210u;
        void *t = xd3d::make_hle_thunk(row(0, "mixed", (void *)impl_2, "s ecx", true), &err);
        check(t != nullptr, "the thunk for \"s ecx\" is built");
        do_call(t, 1, &g_io);
        check_eq(g_args_seen[0], 0x0BADF00D, "the stack argument arrived");
        check_eq(g_args_seen[1], 0x76543210, "and the register argument with it");
        check_eq(g_io.out[1], g_io.out[0], "and the stack is balanced");
    }

    // A void implementation: the caller had a value in eax and must get it back, because an LTCG caller
    // can be keeping something there across the call.
    {
        reset_call();
        g_io.stack[0] = 0x00000007u;
        g_io.stack[1] = 0x00000009u;
        void *t = xd3d::make_hle_thunk(row(0, "void2", (void *)impl_void_2, "s s", false), &err);
        check(t != nullptr, "the thunk for a void implementation is built");
        do_call(t, 2, &g_io);
        check_eq(g_argc_seen, 2, "it saw both arguments");
        check_eq(g_args_seen[0], 7, "first");
        check_eq(g_args_seen[1], 9, "second");
        check_eq(g_io.out[2], 0xAAAAAAAAu, "eax came back with the caller's value, not the callee's");
        check_eq(g_io.out[1], g_io.out[0], "and the stack is balanced");
    }

    // No arguments at all: `ret`, not `ret imm16`, and nothing pushed.
    {
        reset_call();
        void *t = xd3d::make_hle_thunk(row(0, "none", (void *)impl_0, "", true), &err);
        check(t != nullptr, "the thunk for a function with no parameters is built");
        do_call(t, 0, &g_io);
        check_eq(g_argc_seen, 0, "the implementation was called with nothing");
        check_eq(g_io.out[2], 0x00C0FFEEu, "and its return value came back");
        check_eq(g_io.out[1], g_io.out[0], "with the stack untouched");
    }
}

static void test_emitted_shape()
{
    printf("\nwhat the thunk looks like\n");

    // Decoding the first bytes is not the point of the forwarding tests above -- those call through real
    // code -- but a regression that emitted, say, `ret` instead of `ret imm16` would only show up on the
    // *next* call, so the shape is pinned too.
    std::string err;
    void *t = xd3d::make_hle_thunk(row(0, "shape", (void *)impl_3, "s s s", true), &err);
    check(t != nullptr, "a thunk to inspect");
    if (t) {
        const uint8_t *p = (const uint8_t *)t;
        const size_t size = xd3d::hle_thunk_size(row(0, "shape", (void *)impl_3, "s s s", true));
        check_eq(p[0], 0x51, "it pushes ecx first");
        check_eq(p[1], 0x52, "then edx");
        check_eq(p[2], 0xFF, "then copies the first argument off the frame");
        check_eq(p[3], 0x74, "with the short form");
        check_eq(p[4], 0x24, "addressed relative to esp");
        // The tail: the two register pops, then ret imm16 dropping exactly three words.
        check_eq(p[size - 3], 0xC2, "it ends in ret imm16, because the caller pushed arguments");
        const uint16_t drop = (uint16_t)(p[size - 2] | (uint16_t)(p[size - 1] << 8));
        check_eq(drop, 12, "dropping exactly three words");
        check_eq(p[size - 4], 0x59, "ecx is popped just before it, the last of the two saves");
        check_eq(p[size - 5], 0x5A, "and edx before that, matching the push order");
    }

    void *v = xd3d::make_hle_thunk(row(0, "shape2", (void *)impl_0, "", true), &err);
    if (v) {
        const uint8_t *p = (const uint8_t *)v;
        const size_t size = xd3d::hle_thunk_size(row(0, "shape2", (void *)impl_0, "", true));
        check_eq(p[size - 1], 0xC3, "a thunk with no arguments ends in a plain ret");
    }
}

static void test_refusals()
{
    printf("\nwhat the builder refuses\n");

    std::string err;
    check(xd3d::make_hle_thunk(row(0, "bad", (void *)impl_1, "s bogus", true), &err) == nullptr,
          "a spec with an unknown token produces no thunk");
    check(err.find("bogus") != std::string::npos, "and says which token: " + err);

    err.clear();
    check(xd3d::make_hle_thunk(row(0, "noimpl", nullptr, "s", true), &err) == nullptr,
          "a row with no implementation produces no thunk");
    check(err.find("no implementation") != std::string::npos, "and says so: " + err);

    err.clear();
    // Longer than the sixteen parameters the builder accepts: refused rather than written past its buffer.
    const char *huge = "s s s s s s s s s s s s s s s s s s s s";
    check(xd3d::make_hle_thunk(row(0, "huge", (void *)impl_1, huge, true), &err) == nullptr,
          "an absurdly long spec produces no thunk");
    check(err.find("more than") != std::string::npos, "and says it was too long: " + err);

    check_eq(xd3d::hle_thunk_size(row(0, "bad", (void *)impl_1, "s bogus", true)), 0,
             "and its size is reported as zero rather than guessed");
}

int main()
{
    printf("BLACK loader -- the HLE thunk builder\n");

    test_spec_parsing();
    test_forwarding();
    test_emitted_shape();
    test_refusals();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}