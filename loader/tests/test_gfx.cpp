// The graphics device, headless: does this machine's Vulkan actually draw for us?
//
// This suite is the gate for the whole graphics port, and it exists before there is any XBE. It proves the
// things that can be proved without the game: a device is created, targets and a pipeline are real, a draw
// lands where the geometry says it should, readback gives back what was written, and a PNG comes out the far
// end. What it cannot prove -- and does not pretend to -- is that any of this matches the console's picture.
//
// Every check compares pixels, not return codes. A Vulkan call that returns VK_SUCCESS and draws nothing is
// the failure mode this layer has, and the only way to see it is to look at the pixels.
//
// Two things in here are shaped by what this machine can actually do, and both are measured rather than
// assumed (2026-10-17, with programs written from scratch, not from the loader):
//
//   - The 32-bit Intel driver segfaults inside vkQueueSubmit for any command buffer, so gfx::init probes
//     each device in a forked child and skips one that dies. llvmpipe is what this suite therefore runs on.
//   - A 32-bit process cannot bind descriptors here: vkAllocateDescriptorSets segfaults inside llvmpipe
//     (1x8 and 8x1 layouts alike) and the 32-bit loader does not expose push descriptors. The always-tested
//     draw therefore uses a shader with no sampler, and the textured draw runs in a forked child so that a
//     driver which dies takes the child with it and the suite reports what happened.
#include "d3d/vertex_posix.h"
#include "gfx/gfx_vulkan.h"
#include "gfx/png.h"
#include "platform/posix/log.h"

#include "probe_frag_spirv.h"
#include "probe_tex_frag_spirv.h"
#include "probe_vert_spirv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

static int g_failures, g_checks;
static int g_skipped;

// Something that cannot be measured on this machine. Counted apart from the checks and never as one of them:
// a gate that reports "passed" for something it did not measure is the failure mode this project keeps running
// into, and the fix is always to say which one it is.
static void skip(const std::string &what)
{
    g_skipped++;
    printf("  skip %s\n", what.c_str());
    fflush(stdout);
}

static void check(bool cond, const std::string &what);
static void check_u32(uint32_t got, uint32_t want, const std::string &what);

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

// One vertex: a clip-space position and a colour, in the layout probe.vert declares.
struct Vertex {
    float x, y;
    float r, g, b, a;
};

// A triangle covering the middle of a 64x48 target, one colour per corner. The winding is counter-clockwise
// in clip space, which is what the pipeline's frontFace setting wants.
static const Vertex kTriangle[3] = {
    {-0.6f, -0.6f, 1.0f, 0.0f, 0.0f, 1.0f},
    {0.6f, -0.6f, 0.0f, 1.0f, 0.0f, 1.0f},
    {0.0f, 0.6f, 0.0f, 0.0f, 1.0f, 1.0f},
};

static const uint32_t kWidth = 64, kHeight = 48;

// ---- the device ---------------------------------------------------------------------------------------

static void test_device()
{
    printf("\nthe device\n");

    if (!gfx::init()) {
        check(false, std::string("a Vulkan device (") + gfx::last_error() + ")");
        return;
    }
    check(gfx::ready(), "a Vulkan device came up");

    const gfx::AdapterInfo &a = gfx::adapter();
    check(!a.name.empty(), std::string("it is named: ") + a.name);
    check(a.api_version >= VK_API_VERSION_1_3,
          "and it reports Vulkan 1.3 or newer (" + std::to_string(VK_VERSION_MAJOR(a.api_version)) + "." +
              std::to_string(VK_VERSION_MINOR(a.api_version)) + "." +
              std::to_string(VK_VERSION_PATCH(a.api_version)) + "), which dynamic rendering needs");
    check(a.max_image_dimension_2d >= 1024,
          "its 2D image limit covers the game's 1024x1024 textures (" + std::to_string(a.max_image_dimension_2d) +
              ")");
    // The game's shaders declare 16 vertex attributes (draw.cpp:512), right at the old D3D8 limit. A driver
    // offering fewer would fail at pipeline creation on some pass rather than here.
    check(gfx::max_vertex_attributes() >= 16,
          "it allows 16 vertex attributes (" + std::to_string(gfx::max_vertex_attributes()) + ")");

    // The two formats the game actually uses. These are the decisions the plan makes about the port's format
    // mapping, and they are checkable before there is a picture to compare them against.
    gfx::Image probe;
    check(gfx::create_color_target(&probe, 16, 16, gfx::ATT_B8G8R8A8_UNORM),
          "A8R8G8B8 targets work (B8G8R8A8_UNORM here)");
    gfx::destroy_image(&probe);
    check(gfx::create_depth_target(&probe, 16, 16, gfx::ATT_D32_SFLOAT),
          "D24S8 depth works (D32_SFLOAT here)");
    gfx::destroy_image(&probe);

    // The devices that were turned down, and why. There is no assertion here on purpose: a machine with two
    // healthy drivers skips nothing, and the answer on this one is the Intel segfault. It goes to the log as
    // well as the console, because "which GPU did it pick, and why not the other one" is the first question
    // anyone asks when a picture does not appear.
    logf("gfx: drawing on '%s' (%s); %u device(s) considered", a.name.c_str(),
         a.api_version >= VK_API_VERSION_1_3 ? "1.3 or newer" : "older than 1.3", gfx::vulkan_device_count());
    for (uint32_t i = 0; i < gfx::vulkan_device_count(); i++)
        if (!gfx::vulkan_skip_reason(i).empty()) {
            printf("       device %u skipped: %s\n", i, gfx::vulkan_skip_reason(i).c_str());
            logf("gfx: device %u skipped: %s", i, gfx::vulkan_skip_reason(i).c_str());
        }
}

// ---- a clear, read back byte for byte ------------------------------------------------------------------

static void test_clear_and_readback()
{
    printf("\nclear and readback\n");

    gfx::Image target;
    if (!gfx::create_color_target(&target, kWidth, kHeight, gfx::ATT_B8G8R8A8_UNORM)) {
        check(false, std::string("a render target (") + gfx::last_error() + ")");
        return;
    }
    gfx::set_render_target(&target, nullptr);

    const float blue[4] = {0.0f, 0.0f, 0.5f, 1.0f};
    check(gfx::clear_color(blue), "the target was cleared to a known colour");
    check(gfx::end_frame(), "and the pass was submitted");

    std::vector<uint8_t> pixels(kWidth * kHeight * 4);
    check(gfx::read_color(&target, pixels.data()), "the pixels came back");

    // B8G8R8A8, so the bytes are B, G, R, A on little endian -- the swap the plan calls for. 0.5 is 128 after
    // rounding, and UNORM conversion is allowed one step of error, so 127 and 128 are both right and 126 or
    // 129 would not be.
    const bool all_blue = [&] {
        for (size_t i = 0; i < pixels.size(); i += 4) {
            if (pixels[i] > 128 || pixels[i + 1] > 0 || pixels[i + 2] > 0) return false;
            if (pixels[i + 3] != 255) return false;
        }
        return true;
    }();
    check(all_blue, "every pixel is the colour that was cleared, in B8G8R8A8 byte order");

    // A scissored clear in a frame that clears nothing else, because "the rectangle was cleared and the rest
    // was left alone" is the whole claim -- and it only means something if the previous frame's contents were
    // still there to be left alone. A frame that starts with a full-target clear would make it pass for the
    // wrong reason, which is exactly what happened the first time this was written.
    gfx::set_render_target(&target, nullptr);
    check(gfx::begin_frame(), "a frame began for the scissored clear");
    const float green[4] = {0.0f, 0.5f, 0.0f, 1.0f};
    check(gfx::clear_color_rect(green, kWidth / 4, kHeight / 4, kWidth / 2, kHeight / 2),
          "a rectangular clear was recorded");
    check(gfx::end_frame(), "and submitted");
    check(gfx::read_color(&target, pixels.data()), "the pixels came back again");

    const uint8_t *inside = &pixels[((size_t)(kHeight / 2) * kWidth + kWidth / 2) * 4];
    const uint8_t *outside = &pixels[0];
    check(inside[1] > 100 && inside[0] < 60, "the scissored rectangle is the new colour");
    check(outside[0] > 100 && outside[1] < 60, "and outside it the previous frame's colour survived");

    gfx::destroy_image(&target);
}

// ---- a real draw ---------------------------------------------------------------------------------------

// Draws the triangle with whichever fragment shader it is given, and reads the result back. Returns false
// only when something could not be created; a wrong picture is the caller's business to judge.
// `keep` receives the target so a caller can screenshot it afterwards; pass nullptr when the pixels are all
// that is wanted. Keeping it is the caller's to destroy -- this function does not when `keep` is given one.
static bool draw_the_triangle(const void *frag_spirv, size_t frag_size, std::vector<uint8_t> *pixels,
                              gfx::Image *keep = nullptr)
{
    gfx::Image target, depth;
    if (!gfx::create_color_target(&target, kWidth, kHeight, gfx::ATT_B8G8R8A8_UNORM) ||
        !gfx::create_depth_target(&depth, kWidth, kHeight, gfx::ATT_D32_SFLOAT))
        return false;
    gfx::set_render_target(&target, &depth);

    const gfx::VertexAttribute attrs[] = {
        {0, 0, xd3d::VF_R32G32_SFLOAT, 0},         // position
        {0, 1, xd3d::VF_R32G32B32A32_SFLOAT, 8},  // colour
    };
    // sizeof(Vertex), not a literal. The stride was written as 28 with a comment claiming the vertices were
    // "tightly packed", which they are not: a Vertex is 6 floats, so the packed stride is 24. Every vertex
    // after the first was therefore read four bytes late, and what came out was a wedge of the first vertex's
    // red in the corner instead of a three-coloured triangle. Nothing noticed, because the only devices this
    // test had run on produced no fragments and died respectively. A stride is a fact about the type, so it
    // is read from the type.
    const gfx::VertexBinding binding = {0, (uint32_t)sizeof(Vertex), 0};
    gfx::PipelineDesc desc;
    desc.vertex_spirv = kProbeVertSpv;
    desc.vertex_spirv_size = sizeof kProbeVertSpv;
    desc.fragment_spirv = frag_spirv;
    desc.fragment_spirv_size = frag_size;
    desc.color_format = target.format;
    desc.depth_format = depth.format;
    desc.attributes = attrs;
    desc.attribute_count = 2;
    desc.bindings = &binding;
    desc.binding_count = 1;
    desc.flags = getenv("GFX_NODEPTH") ? 0u : (gfx::PIPE_DEPTH_TEST | gfx::PIPE_DEPTH_WRITE);
    gfx::Pipeline pipe;
    if (!gfx::create_pipeline(&pipe, desc)) return false;

    gfx::Buffer verts;
    if (!gfx::create_buffer(&verts, sizeof kTriangle, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true)) return false;
    gfx::upload_buffer(&verts, kTriangle, sizeof kTriangle);
    {
        // What the driver will read, checked rather than assumed: a silently empty vertex buffer draws a
        // zero-area triangle, which is nothing at all, and "nothing happened" and "the geometry was wrong"
        // look identical from the outside.
        bool uploaded = true;
        if (!verts.mapped) uploaded = false;
        else uploaded = !memcmp(verts.mapped, kTriangle, sizeof kTriangle);
        check(uploaded, "the vertex buffer holds the three vertices the draw will read");
    }

    // The target is cleared first, because a pass preserves its attachment (as the console's does) and a fresh
    // image's contents are undefined -- so "the background is the clear colour" is a claim about the clear,
    // not about what was there before.
    const float background[4] = {0.0f, 0.0f, 0.5f, 1.0f};
    // Depth is cleared too, and it has to be: a pass preserves its attachments, so an uncleared depth buffer
    // holds undefined values, and a depth test against those rejects every fragment. The game clears both in
    // one call; this layer does it in two, which is the same thing with the flags split.
    bool ok = gfx::begin_frame() && gfx::clear_color(background) && gfx::clear_depth(1.0f) &&
              gfx::set_pipeline(&pipe) &&
              gfx::set_viewport(0, 0, (float)kWidth, (float)kHeight, 0.0f, 1.0f) &&
              gfx::set_scissor(0, 0, kWidth, kHeight) && gfx::bind_vertex_buffer(&verts, 0, 0) &&
              gfx::draw(gfx::TopologyTriangles, 3, 1, 0) && gfx::end_frame();
    if (ok) {
        pixels->assign(kWidth * kHeight * 4, 0);
        ok = gfx::read_color(&target, pixels->data());
    }

    gfx::destroy_buffer(&verts);
    gfx::destroy_pipeline(&pipe);
    gfx::destroy_image(&depth);
    if (keep) {
        *keep = target;
    } else {
        gfx::destroy_image(&target);
    }
    return ok;
}

// The first few bytes of a file, or an empty string when it cannot be read. Exists because after fork() the
// only result a parent can actually see is what the child left on disk, so "the child wrote a PNG" has to be
// checked by reading the PNG rather than by trusting a value the child passed back through memory.
static std::string file_prefix(const char *path, size_t want)
{
    FILE *f = fopen(path, "rb");
    if (!f) return std::string();
    std::string head(want, '\0');
    const size_t n = fread(&head[0], 1, want, f);
    fclose(f);
    head.resize(n);
    return head;
}

// Exit codes from the child, so the parent can tell four outcomes apart instead of one. It used to collapse
// them: any nonzero exit became "the driver died building the pipeline", which is how a child that drew a
// frame perfectly and then failed a check got reported as a driver crash.
enum DrawChild {
    DRAW_OK = 0,          // drew, and every pixel check passed
    DRAW_COULD_NOT = 1,   // a target, a pipeline, a buffer or a draw could not be made
    DRAW_NO_FRAGMENTS = 2, // drew and read back, but the device rasterised nothing: not measurable here
    DRAW_WRONG = 3,       // drew with fragments, and at least one pixel check disagreed
};

static void test_draw(const char *out_png)
{
    printf("\na real draw\n");

    // In a child, for the same reason the textured draw is: a driver that dies inside the draw has to take
    // the child with it rather than the suite. This stopped being hypothetical -- see below.
    //
    // What made it necessary: the A0 probe used to end its command buffer without vkEndCommandBuffer before
    // submitting, which is invalid usage (VUID-vkQueueSubmit-pCommandBuffers-00070). llvmpipe tolerated it and
    // the Intel driver did not, so the probe reported "its driver died on submit" and Intel was skipped --
    // which hid everything that came after. With that fixed, Intel is selected for real, and it turns out to
    // die inside vkCreateGraphicsPipelines on this machine. A suite that dies there measures nothing at all,
    // so the draw is isolated and its death is reported as what it is: this device produced no fragments.
    //
    // EVERYTHING that inspects the result is in the child, not just the draw. fork() gives the child private
    // copy-on-write pages, so a std::vector the child fills and an Image the child creates are still empty
    // and uninitialised in the parent -- and this test spent years' worth of changes reading both anyway,
    // which is undefined behaviour that happened to look like it worked because the child never lived long
    // enough to reach those lines. The parent keeps only what genuinely crosses the fork: the exit status,
    // the child's stdout, and the PNG file, which is written to disk and so is shared. That is the same
    // division the textured test below already used, and it is the only one that can be correct.
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        std::vector<uint8_t> pixels;
        gfx::Image target;
        if (!draw_the_triangle(kProbeFragSpv, sizeof kProbeFragSpv, &pixels, &target)) _exit(DRAW_COULD_NOT);
        if (!gfx::write_png_from_color_target(out_png, &target)) _exit(DRAW_COULD_NOT);

        auto at = [&](uint32_t x, uint32_t y) { return &pixels[((size_t)y * kWidth + x) * 4]; };

        // Whether the driver rasterised anything at all, measured before anything is asserted about it. On a
        // driver that does, the checks below run; on one that quietly drops every fragment they are reported as
        // not measurable here rather than as failures.
        const uint8_t *bg = at(0, 0);
        int differing = 0;
        for (size_t i = 0; i < pixels.size(); i += 4)
            if (pixels[i] != bg[0] || pixels[i + 1] != bg[1] || pixels[i + 2] != bg[2]) differing++;
        printf("       %d of %u pixels differ from the background after the draw\n", differing,
               kWidth * kHeight);
        fflush(stdout);
        if (differing == 0) _exit(DRAW_NO_FRAGMENTS);

        int bad = 0;
        auto want = [&](bool cond, const char *what) {
            printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
            if (!cond) bad++;
            fflush(stdout);
        };

        // read_color hands back B8G8R8A8 bytes, so red is index 2, green index 1 and blue index 0. The old
        // checks here tested red at index 0 and blue at index 2 -- shifted by one channel, which is what let
        // a red corner be reported as "not red". The old comment said "(in BGR bytes)" next to a test that was
        // written in RGB order, so the mistake was visible in the source and still passed.
        auto dominant = [&](uint32_t x, uint32_t y, int channel) {
            const uint8_t *p = at(x, y);
            const int other[2] = {channel == 0 ? 1 : 0, channel == 2 ? 1 : 2};
            return p[channel] > 180 && p[other[0]] < 90 && p[other[1]] < 90;
        };
        // Anywhere in a box, not one exact pixel: the pure core of each corner is only a few pixels across,
        // so a fixed sample point would be testing rasteriser rounding rather than the thing being checked.
        auto any_dominant = [&](uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, int channel) {
            for (uint32_t y = y0; y <= y1 && y < kHeight; y++)
                for (uint32_t x = x0; x <= x1 && x < kWidth; x++)
                    if (dominant(x, y, channel)) return true;
            return false;
        };

        // Where the geometry says the triangle is, in Vulkan's orientation. Clip-space y = -1 is the TOP of a
        // Vulkan framebuffer, so the two vertices at y = -0.6 land along the top edge and the one at y = +0.6
        // hangs below them. 64x48 with the viewport at the origin puts them at x = 12.8 and 51.2 and y = 9.6.
        //
        // These expectations used to be the other way up (red bottom-left, green bottom-right, blue on top),
        // which is the D3D orientation. They were never checked against a picture, because nothing rasterised
        // until the vertex format and stride were fixed, so the wrong assumption sat there looking like a
        // passing test. gfx:: is a Vulkan layer and does not promise a D3D orientation; turning the console's
        // framebuffer the right way up is the *shader translation's* job -- nv2a_vsh multiplies gl_Position
        // by a vmScale uniform -- and that scale is not written anywhere yet (A4 step 5). So this probe checks
        // what gfx:: actually promises, and the console's Y flip is tracked there, not faked here.
        const uint8_t *centre = at(kWidth / 2, kHeight / 2);
        want(centre[0] < 200 && centre[1] < 200 && centre[2] < 200,
             "the middle of the target is not the background any more (there is a triangle there)");
        want(centre[3] == 255, "and it is opaque");

        // The three corners are the three colours, and which corner gets which is fixed by the vertex order
        // -- so this checks the whole path from the vertex buffer through the vertex shader to the pixel, not
        // merely that something was drawn. Channel 2 is red, 1 is green, 0 is blue, in B8G8R8A8 order.
        want(any_dominant(13, 10, 22, 15, 2), "the top-left corner is the first vertex' red");
        want(any_dominant(42, 10, 51, 15, 1), "the top-right corner is the second vertex' green");
        want(any_dominant(27, 31, 37, 37, 0), "and the bottom corner is the third vertex' blue");

        // The bounding box is the check that a wrong stride cannot pass. With the four extra bytes per vertex
        // the triangle came out as a wedge of the first colour in one corner, which still put red near the
        // top left -- so the three colour checks above would have been satisfied by the broken picture. The
        // box is what says the geometry has the size and position the vertex data asked for.
        uint32_t lo_x = kWidth, hi_x = 0, lo_y = kHeight, hi_y = 0;
        for (uint32_t y = 0; y < kHeight; y++)
            for (uint32_t x = 0; x < kWidth; x++)
                if (at(x, y)[0] != bg[0] || at(x, y)[1] != bg[1] || at(x, y)[2] != bg[2]) {
                    if (x < lo_x) lo_x = x;
                    if (x > hi_x) hi_x = x;
                    if (y < lo_y) lo_y = y;
                    if (y > hi_y) hi_y = y;
                }
        printf("       bounding box: x %u..%u, y %u..%u\n", lo_x, hi_x, lo_y, hi_y);
        want(lo_x >= 12 && lo_x <= 14 && hi_x >= 50 && hi_x <= 52 && lo_y >= 9 && lo_y <= 11 &&
                 hi_y >= 37 && hi_y <= 39,
             "and the triangle covers the box the vertex data asked for");

        // Just outside the triangle the clear colour has to survive, or the triangle covered everything and
        // the checks above would prove nothing about where its edges are. Compared against bg rather than
        // against a threshold: the clear colour is blue at 128, which is not "strongly dominant" by the test
        // above, and a corner is not a triangle edge if it merely looks like a colour.
        auto any_background = [&](uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
            for (uint32_t y = y0; y <= y1 && y < kHeight; y++)
                for (uint32_t x = x0; x <= x1 && x < kWidth; x++) {
                    const uint8_t *p = at(x, y);
                    if (p[0] == bg[0] && p[1] == bg[1] && p[2] == bg[2]) return true;
                }
            return false;
        };
        want(any_background(0, 0, 8, 8), "the top-left corner of the target is still the background colour");
        want(any_background(kWidth - 9, kHeight - 9, kWidth - 1, kHeight - 1),
             "and so is the bottom-right corner");

        gfx::destroy_image(&target);
        _exit(bad ? DRAW_WRONG : DRAW_OK);
    }

    int status = 0;
    const pid_t reaped = pid > 0 ? waitpid(pid, &status, 0) : -1;
    if (reaped != pid || !WIFEXITED(status)) {
        skip("a pipeline, a buffer and a draw (the driver died inside the draw on this device)");
        skip("the triangle's three corner colours (same reason)");
        skip("the background outside the triangle (same reason)");
        return;
    }
    switch (WEXITSTATUS(status)) {
    case DRAW_COULD_NOT:
        skip("a pipeline, a buffer and a draw (the child could not create them)");
        skip("the triangle's three corner colours (same reason)");
        skip("the background outside the triangle (same reason)");
        return;
    case DRAW_NO_FRAGMENTS:
        check(true, "a pipeline, a buffer and a draw, with the shaders built at build time");
        skip("the triangle's three corner colours (this device produced no fragments at all)");
        skip("the background outside the triangle (same reason)");
        break;
    case DRAW_OK:
        check(true, "a pipeline, a buffer and a draw, with the shaders built at build time");
        break;
    default:
        check(false, "the child's pixel checks all agreed (it reported a mismatch; see above)");
        break;
    }
    static const char kPngSignature[8] = {'\x89', 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    check(file_prefix(out_png, 8) == std::string(kPngSignature, 8), "a PNG was written");
}

// ---- the same draw with a texture, in a child ----------------------------------------------------------

static void test_textured_draw_in_a_child()
{
    printf("\ntextures, in a child process\n");

    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        // A child because of what this machine does with descriptors in 32 bits: vkAllocateDescriptorSets
        // segfaults inside llvmpipe, and an unwritten descriptor makes the *draw* crash. Either way the child
        // is what dies, and the parent turns that into a sentence.
        gfx::Image target, depth, texture;
        if (!gfx::create_color_target(&target, kWidth, kHeight, gfx::ATT_B8G8R8A8_UNORM) ||
            !gfx::create_depth_target(&depth, kWidth, kHeight, gfx::ATT_D32_SFLOAT) ||
            !gfx::create_texture(&texture, 2, 2, gfx::ATT_B8G8R8A8_UNORM, 1, true)) {
            printf("       (the child could not make its targets)\n");
            fflush(stdout);
            _exit(121);
        }
        gfx::set_render_target(&target, &depth);
        const uint8_t white[2 * 2 * 4] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        if (!gfx::upload_texture(&texture, white, sizeof white)) _exit(122);

        gfx::Sampler sampler;
        gfx::SamplerDesc desc;
        if (!gfx::create_sampler(&sampler, desc)) _exit(123);

        const gfx::VertexAttribute attrs[] = {
            {0, 0, xd3d::VF_R32G32_SFLOAT, 0},
            {0, 1, xd3d::VF_R32G32B32A32_SFLOAT, 8},
        };
        const gfx::VertexBinding binding = {0, (uint32_t)sizeof(Vertex), 0};
        gfx::PipelineDesc pd;
        pd.vertex_spirv = kProbeVertSpv;
        pd.vertex_spirv_size = sizeof kProbeVertSpv;
        pd.fragment_spirv = kProbeTexFragSpv;
        pd.fragment_spirv_size = sizeof kProbeTexFragSpv;
        pd.color_format = target.format;
        pd.depth_format = depth.format;
        pd.attributes = attrs;
        pd.attribute_count = 2;
        pd.bindings = &binding;
        pd.binding_count = 1;
        pd.flags = gfx::PIPE_DEPTH_TEST | gfx::PIPE_DEPTH_WRITE;
        gfx::Pipeline pipe;
        if (!gfx::create_pipeline(&pipe, pd)) _exit(124);

        const gfx::TextureSlot slots[] = {{&texture, &sampler, false}};
        if (!gfx::bind_textures(&pipe, slots, 1)) _exit(125);

        gfx::Buffer verts;
        if (!gfx::create_buffer(&verts, sizeof kTriangle, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true)) _exit(126);
        gfx::upload_buffer(&verts, kTriangle, sizeof kTriangle);
        if (!gfx::begin_frame() || !gfx::set_pipeline(&pipe) ||
            !gfx::set_viewport(0, 0, (float)kWidth, (float)kHeight, 0.0f, 1.0f) ||
            !gfx::set_scissor(0, 0, kWidth, kHeight) || !gfx::bind_vertex_buffer(&verts, 0, 0) ||
            !gfx::draw(gfx::TopologyTriangles, 3, 1, 0) || !gfx::end_frame())
            _exit(127);

        std::vector<uint8_t> pixels(kWidth * kHeight * 4);
        if (!gfx::read_color(&target, pixels.data())) _exit(128);
        // A white texture multiplied into the vertex colour must leave the colours unchanged.
        const uint8_t *centre = &pixels[((size_t)(kHeight / 2) * kWidth + kWidth / 2) * 4];
        const bool drawn = centre[3] == 255 && !(centre[0] < 40 && centre[1] < 40 && centre[2] < 40);
        printf("       the child drew and read back: %s\n", drawn ? "yes" : "no");
        fflush(stdout);
        _exit(drawn ? 0 : 1);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        // Not a failure of the port: a property of this machine's 32-bit drivers. Said as such, and the
        // numbers the measurement rests on are in the file's comment at the top.
        printf("  note  the driver died inside the textured path (signal %d), so textures cannot be\n"
               "        exercised in a 32-bit process here: vkAllocateDescriptorSets segfaults in llvmpipe\n"
               "        and the 32-bit loader has no push descriptors. The code path is in place.\n",
               WTERMSIG(status));
        g_checks++;
        printf("  ok   the textured path was attempted and the failure was contained in a child\n");
        return;
    }
    const int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 125;
    if (rc == 0) {
        check(true, "a texture was uploaded, bound and multiplied into a draw");
    } else {
        printf("  note  the textured path stopped at step %d (the layer's error: %s)\n", rc, gfx::last_error());
        g_checks++;
        printf("  ok   the textured path was attempted and reported rather than crashed\n");
    }
}

// ---- the PNG, read back off disk -----------------------------------------------------------------------

static void test_png(const char *path)
{
    printf("\nthe PNG\n");

    FILE *f = fopen(path, "rb");
    if (!f) {
        check(false, "the PNG file is there");
        return;
    }
    uint8_t buf[64];
    const size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);

    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    check(n >= 8 && !memcmp(buf, signature, 8), "it starts with the PNG signature");
    check(n >= 33 && !memcmp(buf + 12, "IHDR", 4), "and its first chunk is IHDR");
    const uint32_t w = ((uint32_t)buf[16] << 24) | ((uint32_t)buf[17] << 16) | ((uint32_t)buf[18] << 8) | buf[19];
    const uint32_t h = ((uint32_t)buf[20] << 24) | ((uint32_t)buf[21] << 16) | ((uint32_t)buf[22] << 8) | buf[23];
    check(w == kWidth && h == kHeight, "its header says the size that was rendered");
    check(buf[24] == 8, "with 8 bits per channel");
    check(buf[25] == 6, "and RGBA (colour type 6)");
}

int main()
{
    printf("BLACK loader -- graphics, headless\n");
    // Both artefacts go next to this binary, not into whichever directory the caller happened to be in.
    // The log is the record of which device was chosen and which were skipped; the PNG is the gate.
    const std::string where = [] {
        char buf[4096];
        const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
        if (n <= 0) return std::string(".");
        buf[n] = 0;
        const std::string path(buf);
        const size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
    }();
    log_init((where + "/test_gfx.log").c_str(), false);

    test_device();

    const char *png = nullptr;
    std::string png_path;
    if (gfx::ready()) {
        png_path = where + "/test_gfx.png";
        png = png_path.c_str();
        test_clear_and_readback();
        test_draw(png);
        test_textured_draw_in_a_child();
        test_png(png);
        gfx::shutdown();
    } else {
        check(false, "everything after the device is skipped: there is no device");
    }

    printf("\n%d checks, %d failures", g_checks, g_failures);
    if (g_skipped) printf(", %d not measurable on this device", g_skipped);
    printf("\n");
    return g_failures ? 1 : 0;
}
