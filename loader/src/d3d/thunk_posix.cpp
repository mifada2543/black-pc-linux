#include "d3d/thunk_posix.h"

#include "platform/posix/log.h"

#include <stdio.h>
#include <string.h>

namespace xd3d {
namespace {

// x86 push encodings are 0x50 + register number, in the order eax, ecx, edx, ebx, esp, ebp, esi, edi.
struct RegName {
    const char *name;
    uint8_t code;   // the encoding
    bool pushable;  // false for esp: `push esp` pushes the old esp, not the value
};

const RegName kRegs[] = {
    {"eax", 0x50, true}, {"ecx", 0x51, true}, {"edx", 0x52, true}, {"ebx", 0x53, true},
    {"esp", 0x54, false}, {"ebp", 0x55, true}, {"esi", 0x56, true}, {"edi", 0x57, true},
};

const RegName *find_reg(const char *name)
{
    for (const RegName &r : kRegs)
        if (strcmp(r.name, name) == 0) return &r;
    return nullptr;
}

constexpr int kMaxParams = 16;   // the longest real spec is eight "s"; sixteen leaves room and bounds the maths
constexpr size_t kMaxThunk = 128; // comfortably above the longest spec, and small enough to be one cache line

struct Token {
    bool is_stack;
    const RegName *reg;
    int stack_index;
};

// Split the spec into tokens: "s" for a stack argument, a register name otherwise. Whitespace and commas
// separate, because the tables were written both ways.
bool parse_spec(const char *params, Token *tokens, int &count, int &stack_args, char *error, size_t error_size)
{
    count = 0;
    stack_args = 0;
    char tok[16];
    size_t used = 0;
    for (const char *p = params; ; p++) {
        const char c = *p;
        if (c == ' ' || c == ',' || c == '\t' || c == 0) {
            if (used) {
                tok[used] = 0;
                if (count == kMaxParams) {
                    snprintf(error, error_size, "more than %d parameters", kMaxParams);
                    return false;
                }
                if (strcmp(tok, "s") == 0) {
                    tokens[count] = Token{true, nullptr, stack_args++};
                } else if (const RegName *r = find_reg(tok)) {
                    if (!r->pushable) {
                        snprintf(error, error_size, "'%s' cannot be forwarded (push esp pushes the old esp)", tok);
                        return false;
                    }
                    tokens[count] = Token{false, r, -1};
                } else {
                    snprintf(error, error_size, "unknown parameter '%s' (expected s or a register name)", tok);
                    return false;
                }
                count++;
                used = 0;
            }
            if (c == 0) break;
        } else {
            if (used + 1 >= sizeof tok) {
                snprintf(error, error_size, "parameter name too long");
                return false;
            }
            tok[used++] = c;
        }
    }
    return true;
}

} // namespace

ThunkPlan plan_hle_thunk(const char *params, bool returns)
{
    ThunkPlan plan;
    plan.pushes_eax = !returns;
    Token tokens[kMaxParams];
    int count = 0, stack_args = 0;
    char error[96] = {0};
    if (!parse_spec(params ? params : "", tokens, count, stack_args, error, sizeof error)) {
        snprintf(plan.error, sizeof plan.error, "%s", error);
        return plan;
    }
    plan.stack_args = stack_args;
    plan.valid = true;
    return plan;
}

size_t hle_thunk_size(const Hle &h)
{
    const ThunkPlan plan = plan_hle_thunk(h.params, h.returns);
    if (!plan.valid) return 0;

    // Counted rather than guessed, and the count is what the size check uses.
    Token tokens[kMaxParams];
    int count = 0, stack_args = 0;
    char error[96] = {0};
    if (!parse_spec(h.params, tokens, count, stack_args, error, sizeof error)) return 0;

    size_t n = 0;
    n += 2;                                      // push ecx, push edx
    if (!h.returns) n += 1;                      // push eax (and a matching pop later)
    for (int i = 0; i < count; i++) n += tokens[i].is_stack ? 4 : 1;
    n += 5;                                      // mov eax, impl
    n += 2;                                      // call eax
    if (!h.returns) n += 1;                      // pop eax
    n += 2;                                      // pop edx, pop ecx
    n += stack_args ? 3 : 1;                     // ret imm16, or ret
    return n;
}

void *make_hle_thunk(const Hle &h, std::string *err)
{
    auto fail = [&](const char *why) -> void * {
        if (err) *err = why;
        return nullptr;
    };
    if (!h.impl) return fail("no implementation bound");

    Token tokens[kMaxParams];
    int count = 0, stack_args = 0;
    char error[96] = {0};
    if (!parse_spec(h.params, tokens, count, stack_args, error, sizeof error)) {
        char why[192];
        snprintf(why, sizeof why, "%s: %s", h.name ? h.name : "?", error);
        return fail(why);
    }

    // The count first, so a long spec is refused instead of running off the end of a fixed buffer.
    const size_t size = hle_thunk_size(h);
    if (!size || size > kMaxThunk) {
        char why[192];
        snprintf(why, sizeof why, "%s: the thunk for this spec does not fit (%zu bytes)",
                 h.name ? h.name : "?", size);
        return fail(why);
    }

    uint8_t buf[kMaxThunk];
    size_t n = 0;
    buf[n++] = 0x51; // push ecx
    buf[n++] = 0x52; // push edx
    int saved = 2;
    if (!h.returns) {
        buf[n++] = 0x50; // push eax
        saved = 3;
    }

    // Arguments are pushed back to front, because the implementation is __stdcall: it pops its own
    // parameters, so the first parameter has to end up on top of the stack. A stack argument is copied out
    // of the caller's frame, offset by what has been pushed so far -- the saved registers and the
    // arguments already queued -- plus the return address at the bottom.
    int pushed = 0;
    for (int i = count - 1; i >= 0; i--) {
        if (tokens[i].is_stack) {
            const int off = 4 + 4 * tokens[i].stack_index + 4 * (saved + pushed);
            if (off < 128) {
                buf[n++] = 0xFF; // push dword [esp+off8]
                buf[n++] = 0x74;
                buf[n++] = 0x24;
                buf[n++] = (uint8_t)off;
            } else {
                buf[n++] = 0xFF; // push dword [esp+off32]
                buf[n++] = 0xB4;
                buf[n++] = 0x24;
                const int32_t off32 = off;
                memcpy(buf + n, &off32, 4);
                n += 4;
            }
        } else {
            buf[n++] = tokens[i].reg->code; // push <reg>
        }
        pushed++;
    }

    buf[n++] = 0xB8; // mov eax, impl
    const uint32_t impl = (uint32_t)(uintptr_t)h.impl;
    memcpy(buf + n, &impl, 4);
    n += 4;
    buf[n++] = 0xFF; // call eax
    buf[n++] = 0xD0; // the implementation is __stdcall: it pops its own parameters
    if (!h.returns) buf[n++] = 0x58; // pop eax: a void function may leave anything in it, and the caller
    buf[n++] = 0x5A;                 // pop edx
    buf[n++] = 0x59;                 // pop ecx
    if (stack_args) {
        // Exactly the words the caller pushed. Not "about that many": the stack has to be where the
        // caller expects it, and this is the number the invoice's four past bug-classes all came down to.
        buf[n++] = 0xC2; // ret imm16
        const uint16_t drop = (uint16_t)(stack_args * 4);
        memcpy(buf + n, &drop, 2);
        n += 2;
    } else {
        buf[n++] = 0xC3; // ret: nothing was pushed on the stack to drop
    }
    if (n != size) {
        char why[192];
        snprintf(why, sizeof why, "%s: internal size mismatch (%zu counted, %zu emitted)",
                 h.name ? h.name : "?", size, n);
        return fail(why);
    }

    std::string alloc_err;
    uint8_t *p = (uint8_t *)hooks_alloc_thunk(n, alloc_err);
    if (!p) {
        if (err) *err = alloc_err;
        return nullptr;
    }
    memcpy(p, buf, n);
    return p;
}

} // namespace xd3d