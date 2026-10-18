// D3DPOOL_MANAGED: the host block, the device copy, and the dirty range between them.
//
// Vulkan has no managed memory, so the console's most convenient property -- the game's pointer is the
// memory the GPU reads -- has to be rebuilt out of two copies. What makes that affordable rather than a
// per-frame stall is the dirty range: a game that locks a vertex buffer and rewrites one triangle must not
// pay for the whole scene.
//
// The checks are about bytes, not flags: a dirty tracker that reports the right ranges and copies the wrong
// ones is worse than no tracker, because it looks correct. So every step reads the device buffer back.
//
// One thing that shapes this whole file. An upload is recorded into the frame's command buffer, which means
// the bytes do not exist until the frame is submitted. A readback that forgot that returns whatever the
// staging buffer happened to be holding -- and a reused allocation on llvmpipe very often holds the right
// bytes by accident. Two checks here passed that way before `submit_pending` was added, which is the reason
// it is called by name in the readback helper rather than trusted to happen somewhere.
#include "gfx/gfx_vulkan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

static int g_checks = 0, g_failures = 0;

static void check(bool ok, const char *what)
{
    printf("  %s   %s\n", ok ? "ok  " : "FAIL", what);
    g_checks++;
    if (!ok) g_failures++;
}

// Submit whatever the frame has recorded, so the copies in it have actually run. Without this the readback
// below answers a question about memory nobody wrote to.
static void submit_pending()
{
    if (gfx::end_frame()) gfx::begin_frame();
}

// Compare `bytes` at `offset` in the device buffer against `expected`. The offset is a parameter because a
// download always starts at zero, and a check that silently read the wrong bytes would pass whenever the
// wrong bytes happened to match.
static bool device_equals(uint32_t handle, uint32_t offset, const void *expected, uint32_t bytes)
{
    const gfx::ManagedBuffer *m = gfx::managed_buffer_at(handle);
    if (!m) return false;
    submit_pending();
    std::vector<uint8_t> got(bytes, 0xEE); // poison, so an unread buffer cannot look right
    if (!gfx::download_buffer(&m->device, offset, bytes, got.data())) return false;
    return memcmp(got.data(), expected, bytes) == 0;
}

// What a flush owes the caller when there is no frame in place.
//
// The honest answer differs by device and both are correct. A copy that goes through a command buffer needs a
// frame to record into and refuses without one; a copy into host-coherent memory is a memcpy and needs
// nothing at all, and refusing it would be refusing something that already worked. So what is checked is
// the invariant underneath both: never a silent no-op, and never a failure that forgets the range.
static void test_uploading_with_no_frame_is_refused()
{
    printf("\nuploading with no frame in place\n");

    std::vector<uint8_t> host(256, 0x33);
    const char *err = nullptr;
    const uint32_t h = gfx::managed_buffer_create(host.data(), 256, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &err);
    if (!h) {
        check(false, "a managed buffer is created");
        return;
    }

    const bool flushed = gfx::managed_buffer_flush(h, &err);
    gfx::ManagedBuffer *m = gfx::managed_buffer_at(h);
    if (flushed) {
        check(!m->dirty, "a flush that needs no frame succeeds, and says the buffer is clean");
        check(m->ever_uploaded, "and records that the copy happened");
    } else {
        check(err != nullptr, "one that does is refused, with a reason");
        check(m->dirty && m->dirty_lo == 0 && m->dirty_hi == 256, "and the range stays dirty, to be retried");
    }

    gfx::Image target;
    if (gfx::create_color_target(&target, 64, 64, VK_FORMAT_B8G8R8A8_UNORM)) {
        gfx::set_render_target(&target, nullptr);
        check(gfx::managed_buffer_flush(h, nullptr), "with a target set, the same flush succeeds");
        check(device_equals(h, 0, host.data(), 256), "and once the frame is submitted the bytes are there");
        gfx::set_render_target(nullptr, nullptr);
    }
    gfx::managed_buffer_destroy(h);
}

static void test_a_fresh_buffer_starts_dirty()
{
    printf("\na fresh managed buffer\n");

    std::vector<uint8_t> host(1024, 0);
    for (size_t i = 0; i < host.size(); i++) host[i] = (uint8_t)(i * 7 + 1);

    const char *err = nullptr;
    const uint32_t h =
        gfx::managed_buffer_create(host.data(), (uint32_t)host.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &err);
    check(h != 0, "a managed buffer is created");
    if (!h) return;

    gfx::ManagedBuffer *m = gfx::managed_buffer_at(h);
    check(m->host == host.data(), "and remembers the host block it was given");
    check(m->bytes == 1024, "and its size");
    // Everything is dirty at first: the game has been writing this memory since before the loader knew it
    // existed, so a first flush that copied nothing would draw a buffer of zeroes.
    check(m->dirty && m->dirty_lo == 0 && m->dirty_hi == 1024, "and starts entirely dirty");

    check(gfx::managed_buffer_flush(h, &err), "the first flush copies");
    check(device_equals(h, 0, host.data(), 1024), "and the device side holds what the host side held");
    check(!m->dirty, "and the buffer is clean afterwards");
    gfx::managed_buffer_destroy(h);
}

static void test_only_the_dirty_range_is_copied()
{
    printf("\nonly what the game touched\n");

    std::vector<uint8_t> host(1024, 0);
    const uint32_t h = gfx::managed_buffer_create(host.data(), 1024, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, nullptr);
    if (!h) {
        check(false, "a managed buffer is created");
        return;
    }
    gfx::managed_buffer_flush(h, nullptr);
    submit_pending();
    gfx::ManagedBuffer *m = gfx::managed_buffer_at(h);

    // One triangle's worth in the middle of a scene.
    const uint64_t lo = 256, hi = 320;
    memset(host.data() + lo, 0xAB, (size_t)(hi - lo));
    gfx::managed_buffer_mark_dirty(h, lo, hi);
    check(m->dirty && m->dirty_lo == lo && m->dirty_hi == hi, "a write in the middle marks exactly that range");
    check(gfx::managed_buffer_flush(h, nullptr), "and it flushes");
    check(device_equals(h, (uint32_t)lo, host.data() + lo, (uint32_t)(hi - lo)), "and those bytes reached the device");
    check(!m->dirty, "with nothing left dirty");

    // Two writes at opposite ends must produce one copy spanning both. Keeping only the newer one loses the
    // earlier write with nothing to say so -- which is why this is a separate check and not a comment.
    memset(host.data(), 0x11, 16);
    memset(host.data() + 1000, 0x22, 16);
    gfx::managed_buffer_mark_dirty(h, 0, 16);
    gfx::managed_buffer_mark_dirty(h, 1000, 1016);
    check(m->dirty && m->dirty_lo == 0 && m->dirty_hi == 1016,
          "two writes at opposite ends widen the range rather than replacing it");
    gfx::managed_buffer_flush(h, nullptr);
    check(device_equals(h, 0, host.data(), 16), "the first write landed");
    check(device_equals(h, 1000, host.data() + 1000, 16), "and so did the second");
    gfx::managed_buffer_destroy(h);
}

// A second flush after the first must not cost anything, and a write that changes nothing must not mark the
// buffer. Both are the common case between frames, and both turn into a per-frame stall if handled wrong.
static void test_a_clean_buffer_copies_nothing()
{
    printf("\nnothing to do is not a failure\n");

    std::vector<uint8_t> host(256, 0x5A);
    const uint32_t h = gfx::managed_buffer_create(host.data(), 256, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, nullptr);
    if (!h) {
        check(false, "a managed buffer is created");
        return;
    }
    gfx::managed_buffer_flush(h, nullptr);
    submit_pending();

    check(gfx::managed_buffer_flush(h, nullptr), "a flush with nothing dirty succeeds");
    check(gfx::managed_buffer_at(h)->ever_uploaded, "and leaves the buffer marked as uploaded");

    gfx::ManagedBuffer *m = gfx::managed_buffer_at(h);
    gfx::managed_buffer_mark_dirty(h, 100, 100);
    check(!m->dirty, "an empty range marks nothing");
    gfx::managed_buffer_mark_dirty(h, 200, 150); // backwards
    check(!m->dirty, "and neither does one that runs backwards");

    // A whole-buffer mark after a clean flush must not widen from zero. This is the case the explicit
    // `dirty` flag exists for: with "0,0 means clean" the mark produced [0, size) -- correct bytes, full cost.
    gfx::managed_buffer_mark_dirty(h, 64, 128);
    check(m->dirty && m->dirty_lo == 64 && m->dirty_hi == 128, "a mark after a clean flush starts a new range");
    gfx::managed_buffer_flush(h, nullptr);
    gfx::managed_buffer_destroy(h);
}

static void test_it_refuses_what_it_cannot_do()
{
    printf("\nwhat it refuses\n");

    const char *err = nullptr;
    check(gfx::managed_buffer_create(nullptr, 256, 0, &err) == 0 && err != nullptr, "no host memory is refused with a reason");
    check(gfx::managed_buffer_create((void *)"x", 0, 0, &err) == 0 && err != nullptr, "and so is no size");
    check(gfx::managed_buffer_at(0) == nullptr, "handle zero is no buffer");
    check(gfx::managed_buffer_at(99999) == nullptr, "and neither is one past the end");
    check(!gfx::managed_buffer_flush(4242, &err) && err != nullptr, "flushing a handle that is not there is refused");
    gfx::managed_buffer_destroy(0); // must not crash
    gfx::managed_buffer_destroy(99999);
    check(true, "destroying a handle that is not there is harmless");

    // A range past the end of the buffer must not be copied out of bounds. Clamping is right here -- the
    // game wrote within its own allocation and the arithmetic overshot -- but it has to be a clamp.
    std::vector<uint8_t> host(128, 0x77);
    const uint32_t h = gfx::managed_buffer_create(host.data(), 128, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, nullptr);
    if (h) {
        gfx::managed_buffer_flush(h, nullptr);
        submit_pending();
        gfx::managed_buffer_mark_dirty(h, 100, 99999);
        gfx::ManagedBuffer *m = gfx::managed_buffer_at(h);
        check(m->dirty_hi == 128, "a range past the end is clamped to the buffer, not followed");
        gfx::managed_buffer_flush(h, nullptr);
        check(device_equals(h, 100, host.data() + 100, 28), "and what was clamped is what got copied");
        gfx::managed_buffer_destroy(h);
    }
}

int main()
{
    printf("BLACK loader -- managed buffers and dirty tracking\n");
    if (!gfx::init()) {
        printf("  --     no Vulkan on this machine: %s\n", gfx::last_error());
        printf("\n%d checks, 0 failures\n", g_checks);
        return 77;
    }

    gfx::Image target;
    if (!gfx::create_color_target(&target, 64, 64, VK_FORMAT_B8G8R8A8_UNORM)) {
        printf("  --     no render target: %s\n", gfx::last_error());
        gfx::shutdown();
        return 77;
    }

    // Before the target is set, so the first test really does meet a loader with no frame in place.
    test_uploading_with_no_frame_is_refused();

    gfx::set_render_target(&target, nullptr);
    test_a_fresh_buffer_starts_dirty();
    test_only_the_dirty_range_is_copied();
    test_a_clean_buffer_copies_nothing();
    test_it_refuses_what_it_cannot_do();

    gfx::set_render_target(nullptr, nullptr);
    gfx::shutdown();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}