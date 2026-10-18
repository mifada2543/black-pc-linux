// The Vulkan device this port draws with.
//
// Scope, and why it is this shape
// ------------------------------
// The Windows implementation puts Direct3D 9 underneath the Xbox's Direct3D 8 and lets the driver own
// everything else -- render targets, state, shaders. There is no Vulkan equivalent of that arrangement to
// copy, so this layer is deliberately the smallest thing that can draw: a device, buffers, images, one
// pipeline, and a frame recorder. What the D3D8 layer needs is exactly this and nothing more, and the
// decisions below are the ones it constrains:
//
//   - Draw calls come as `DrawPrimitiveUP`/`DrawIndexedPrimitiveUP` with the vertices in system memory
//     (there is no D3D9-style vertex or index buffer anywhere in the game; see draw.cpp). So there are no
//     vertex buffer objects to mirror, only per-draw uploads into a persistently mapped staging buffer.
//   - The formats the game uses are A8R8G8B8 and D24S8, which are B8G8R8A8_UNORM and D32_SFLOAT here. The
//     game reads its own framebuffer contents through LockRect, so readback has to work, not just writes.
//   - `D3DPOOL_MANAGED` does not exist. Textures the game locks get copied through a staging buffer with
//     dirty tracking, which is why every image here can be a copy destination.
//
// Dynamic rendering (no VkRenderPass objects) is used because the loader has exactly one colour attachment
// and at most one depth attachment, and because the D3D8 layer has to be able to change render target
// mid-frame the way SetRenderTarget does. It is core Vulkan from 1.3 on; a driver below that is reported
// as unsupported rather than quietly given a render-pass path it never exercises.
//
// No backend abstraction. This is Vulkan, called directly, and the D3D8 layer above it is written against
// these types.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include <vulkan/vulkan.h>

namespace gfx {

// ---- the device ----

struct AdapterInfo {
    std::string name;
    uint32_t api_version = 0;
    uint32_t driver_version = 0;
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    int device_type = 0; // VkPhysicalDeviceType, kept as an int so callers need no Vulkan headers
    bool present_supported = false;
    uint32_t max_image_dimension_2d = 0;
};

// Create the instance, pick a physical device, create the logical device and one graphics queue. Safe to
// call again: the second call returns what the first did. False means Vulkan is unusable here, and
// last_error() says why in words.
//
// Physical devices are tried in preference order (discrete, integrated, virtual, then software) and each
// one is put through a submit in a forked child before it is accepted. That is not paranoia: on this
// machine the 32-bit Intel driver dies inside vkQueueSubmit for any command buffer at all, and a
// from-scratch ten-line program reproduces it, so picking the GPU by preference alone would take the whole
// loader down at the first frame on a perfectly ordinary machine. A driver that cannot be talked to
// gracefully is skipped with a line in the log saying so.
bool init();

// Force a physical device by name substring (BLACKLINUX_VK_DEVICE=lvp, =0 for the first one) and skip the
// probe entirely (BLACKLINUX_VK_PROBE=0), which is what a developer wants when investigating the probe
// itself. Read once, at the first init().
void vulkan_set_device_override(const char *name_substring);
void vulkan_set_probe_enabled(bool on);
void shutdown();
bool ready();
const char *last_error();

const AdapterInfo &adapter();

// Why a physical device was skipped, by its index in vkEnumeratePhysicalDevices order. Empty when the
// device was accepted, or when it was never considered.
std::string vulkan_skip_reason(uint32_t index);
uint32_t vulkan_device_count();

// Every device on the machine, in enumeration order, filled in even for the ones that were skipped. The A0
// gate reports on all of them one at a time, and `--doctor` needs the same list; a function that only knows
// the device it picked cannot answer "why not the other one".
uint32_t adapter_count();
const AdapterInfo &adapter_at(uint32_t index);

// Which of them the loader actually drew on, or -1 before init.
int adapter_index();

// Draw on device N whatever the preference order says, which is how a gate measures *each* driver instead
// of only the one the loader happens to prefer. -1 restores the normal choice.
void vulkan_set_device_index(int index);
bool has_device_extension(const char *name);

// The objects the D3D8 layer needs for the parts Vulkan keeps to itself: the queue it presents on, and the
// device for image layout transitions it has to make itself when it hands back a texture.
VkQueue queue();
VkDevice device();

// The device's own limits that the D3D8 layer has to respect rather than assume: the game's shaders
// declare 16 vertex attributes and its textures are 1024x1024 with mip chains, and both are stated as
// device facts here instead of as constants in the shader path.
uint32_t max_vertex_attributes();

// ---- memory ----

// A memory type satisfying `want`, or UINT32_MAX. Types come back in preference order: device-local first,
// then host-visible, because a resource that is sampled every frame must not live in system memory.
uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid = 0);

// ---- buffers ----

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = nullptr; // non-null when host-visible, and then persistently mapped
    VkDeviceSize size = 0;
};

// `usage` must include VK_BUFFER_USAGE_TRANSFER_DST for upload_buffer to work.
bool create_buffer(Buffer *b, VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible);
void destroy_buffer(Buffer *b);

// Replace a buffer's contents from host memory. On a device-local buffer this is a staging copy and a
// barrier; on a host-visible one it is a memcpy. `size` may be less than the buffer's, for the partial
// updates LockRect(..., D3DLOCK_DISCARD) turns into on the other side.
// `usage` must include VK_BUFFER_USAGE_TRANSFER_DST for upload_buffer to work.
bool upload_buffer(Buffer *b, const void *data, VkDeviceSize size);

// Upload a *range*, starting at `offset` in the buffer. upload_buffer overwrites the whole thing from zero,
// which is what a fresh upload wants and what a dirty range does not: a D3DPOOL_MANAGED vertex buffer is
// re-locked and partly rewritten many times a frame, and copying all of it each time is the difference
// between a vertex buffer costing nothing and costing a frame's bandwidth.
bool update_buffer(Buffer *b, VkDeviceSize offset, VkDeviceSize size, const void *data);

// Copy a buffer's contents back into host memory, for a caller that has to read what the GPU saw. This is
// how the tests check that an upload landed, and how any path that reads back a resource will do it.
// `b` must have been created with host-visible memory or be copied through staging; either way the answer
// is in `out`.
bool download_buffer(const Buffer *b, VkDeviceSize offset, VkDeviceSize size, void *out);

// ---- D3DPOOL_MANAGED ----
//
// A managed resource is one the game writes through a pointer it holds and the GPU reads through a
// different, device-local copy. Vulkan has no such thing, so the two halves are here: the host block stays
// writable and cheap, and the range the game actually touched is what gets copied across.
//
// The dirty range matters more than it looks. Locking a vertex buffer and rewriting one triangle must not
// cost a copy of the whole scene, and the console got that for free because the memory *was* the memory.
// Tracking the range is what buys it back.

struct ManagedBuffer {
    void *host = nullptr; // what Lock hands the game; the game's own writes land here
    uint32_t bytes = 0;
    Buffer device; // the DEVICE_LOCAL copy the pipeline reads
    uint64_t dirty_lo = 0;
    uint64_t dirty_hi = 0;
    bool dirty = false; // whether the range above means anything; see mark_dirty
    bool ever_uploaded = false;
};

// Create the device-side half. `host` is memory the caller already owns and keeps writing to; this does not
// take ownership of it, because the game's reference to it outlives any pool the loader might have.
uint32_t managed_buffer_create(void *host, uint32_t bytes, VkBufferUsageFlags usage, const char **err);

// Note that the game changed [lo, hi). Ranges accumulate rather than replace: two writes to opposite ends
// of the buffer must produce one copy spanning both, not one copy that loses the first.
void managed_buffer_mark_dirty(uint32_t handle, uint64_t lo, uint64_t hi);

// Copy whatever is dirty across, and forget it. Returns false only when there was something to copy and the
// copy failed; "nothing to do" is a success, because that is the common case between frames.
bool managed_buffer_flush(uint32_t handle, const char **err);

bool managed_buffer_bind(uint32_t handle, uint32_t binding, VkDeviceSize offset);
void managed_buffer_destroy(uint32_t handle);

// For tests and for the draw path: the buffer behind a handle, and the count of them.
ManagedBuffer *managed_buffer_at(uint32_t handle);
size_t managed_buffer_count();

// ---- images ----

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t mip_levels = 1;
    // Index into the loader's table of current image layouts. Vulkan's layout is part of an image's state,
    // not of a render pass, and this port has no render pass objects to remember it in -- see gfx_vulkan.cpp.
    uint32_t layout_slot = 0;
};

// A colour target the loader can render into, sample, and read back. `linear` picks the sRGB view: the
// game writes gamma-corrected values already, so UNORM is the normal case.
bool create_color_target(Image *img, uint32_t width, uint32_t height, VkFormat format, bool linear = false);
bool create_depth_target(Image *img, uint32_t width, uint32_t height, VkFormat format);
bool create_texture(Image *img, uint32_t width, uint32_t height, VkFormat format, uint32_t mip_levels,
                    bool sampled = true);
void destroy_image(Image *img);

// Upload pixels into a texture, or into one level of it. `level` is a mip index.
bool upload_texture(Image *img, const void *pixels, VkDeviceSize bytes, uint32_t level = 0);

// The sRGB view of a colour format, for a target whose values are already gamma-corrected. Written out
// rather than pulled from a helper library: it is four cases, and it is the one place where getting the
// format wrong shows up as a subtly too-bright frame instead of an error.
VkFormat srgb_format(VkFormat f);

// ---- pipeline ----

// The vertex format codes the d3d layer works in, mapped to Vulkan's. The codes are plain integers so
// that d3d/ can name a vertex format without naming a Vulkan type (plan section 3.2); the numbers must
// match VertexFormatCode in d3d/vertex_posix.h, and they are checked here rather than assumed -- a table
// that drifted would produce pipelines that read the right width from the wrong format, which draws.
//
// VF_UNPACKED has no answer here on purpose: NORMPACKED3 is three signed bytes packed into a dword, and
// handing back the nearest RGBA format would fill the fourth component with buffer bytes nobody meant to
// read. It is refused, and the caller has to unpack it in the shader instead.
VkFormat gfx_format_for_vertex_code(uint32_t code);

enum PipelineFlag : uint32_t {
    PIPE_BLEND = 1u << 0,              // alpha blending, the Xbox's default for most passes
    PIPE_DEPTH_TEST = 1u << 1,         // depth test, less-or-equal
    PIPE_DEPTH_WRITE = 1u << 2,        // depth writes
    PIPE_CULL_BACK = 1u << 3,          // cull back faces, the Xbox's default winding
    PIPE_ALPHA_TO_COVERAGE = 1u << 4,  // for targets with MSAA
};

struct VertexAttribute {
    uint32_t binding;
    uint32_t location; // shader input location
    // A VertexFormatCode, not a VkFormat. The D3D layer works in those codes and must not have to name a
    // Vulkan type (plan section 3.2), so the conversion happens here, once, where Vulkan types belong. A
    // caller that passes VK_FORMAT_R32_SFLOAT here would get whatever code that happens to be -- which is
    // why the field is uint32_t and the comment names where the numbering comes from.
    uint32_t format;
    uint32_t offset;
};

// One interleaved vertex stream. The stride is given per binding rather than derived from the attributes,
// because the game's vertex declaration is 16 FLOAT4s and the attributes may or may not cover all of it --
// and because a pipeline with a stride of zero is what a driver turns into a crash rather than an error.
struct VertexBinding {
    uint32_t binding;
    uint32_t stride;
    uint32_t rate; // 0 = per vertex, 1 = per instance
};

struct PipelineDesc {
    // Shader code is bytes, exactly as the SPIR-V files are and as vkCreateShaderModule's codeSize counts
    // them. A word array here would silently multiply every size by four.
    const void *vertex_spirv = nullptr;
    size_t vertex_spirv_size = 0;
    const void *fragment_spirv = nullptr;
    size_t fragment_spirv_size = 0;
    VkFormat color_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkFormat depth_format = VK_FORMAT_UNDEFINED; // VK_FORMAT_UNDEFINED for no depth attachment
    const VertexAttribute *attributes = nullptr;
    uint32_t attribute_count = 0;
    const VertexBinding *bindings = nullptr;
    uint32_t binding_count = 0;
    uint32_t flags = 0;
    uint32_t sample_count = 1; // must match the attachment
};

struct Pipeline {
    VkShaderModule vertex_module = VK_NULL_HANDLE;
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout set0 = VK_NULL_HANDLE; // one combined image sampler per set, see the D3D8 layer
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet bound_set = VK_NULL_HANDLE; // freed and rewritten on each bind_textures
};

bool create_pipeline(Pipeline *p, const PipelineDesc &desc);
void destroy_pipeline(Pipeline *p);

// ---- samplers and texture bindings ----
//
// One sampler per D3DTSS set. The Xbox's sampler state is address modes, filters and anisotropy, which maps
// onto Vulkan's sampler almost one to one; the one thing Vulkan cannot do is clamp a *2D* texture's address
// on the w axis, which is what the mip LOD clamp is for instead.

enum AddressMode { ADDRESS_WRAP, ADDRESS_MIRROR, ADDRESS_CLAMP, ADDRESS_MIRROR_ONCE, ADDRESS_BORDER };
enum FilterMode { FILTER_POINT, FILTER_LINEAR };

struct SamplerDesc {
    AddressMode u = ADDRESS_WRAP;
    AddressMode v = ADDRESS_WRAP;
    AddressMode w = ADDRESS_WRAP;
    FilterMode mag_filter = FILTER_LINEAR;
    FilterMode min_filter = FILTER_LINEAR;
    FilterMode mip_filter = FILTER_LINEAR;
    float lod_bias = 0.0f;
    float max_anisotropy = 1.0f; // 1 = no anisotropic filtering, which is the Xbox's default
    float border[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int max_lod = 0; // 0 here means "as many as the texture has"
};

struct Sampler {
    VkSampler sampler = VK_NULL_HANDLE;
};

// Bindings 0..3 are 2D textures, 4..7 are cube textures. Two ranges rather than one, because Vulkan forbids a
// single descriptor for both image types and the game samples a cube map and a 2D texture from the same
// register (see the nv2a_psh notes in the reference: 92-93).
constexpr uint32_t MAX_TEXTURE_SLOTS = 4;
constexpr uint32_t CUBE_SLOT_BASE = 4;

bool create_sampler(Sampler *s, const SamplerDesc &desc);
void destroy_sampler(Sampler *s);

struct TextureSlot {
    const Image *image = nullptr; // null binds the 1x1 white placeholder, never an unset descriptor
    const Sampler *sampler = nullptr;
    bool cube = false;
};

// Allocate and write the pipeline's set 0, then bind it. Every slot not named here gets the placeholder, so a
// shader that samples a register the game never set reads white instead of whatever a stale descriptor held --
// which on this machine's llvmpipe is a crash rather than a wrong pixel.
// Takes a non-const pipeline because it owns the currently bound set: rebinding frees the old one, and a
// pool that grows with every bind is a pool that runs out in the middle of a frame.
bool bind_textures(Pipeline *p, const TextureSlot slots[], uint32_t count);

// ---- frames ----

// Render targets for the frame in progress. The D3D8 layer calls this from SetRenderTarget, which is why it
// is allowed between draws rather than only at a frame boundary: the loader ends the current pass, starts
// a new one on the new attachments, and continues. `depth` may be null.
void set_render_target(Image *color, Image *depth);

// Begin recording. After this, draw calls record into the current pass. False leaves nothing recorded.
bool begin_frame();
bool set_pipeline(const Pipeline *p);
bool set_viewport(float x, float y, float w, float h, float min_z, float max_z);
bool set_scissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
bool bind_vertex_buffer(const Buffer *b, uint32_t binding, VkDeviceSize offset);
bool bind_index_buffer(const Buffer *b, VkIndexType type);
bool draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex);
bool draw_indexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset);
bool set_scissor_enable(bool on);

// Clear the whole bound target. With no frame in progress this starts one, which is what the "reset the target
// to black" path at device reset needs.
//
// A whole-target clear is expressed as the pass's loadOp rather than as a clear attachment, because that is
// the form a driver's rasteriser agrees with: on 32-bit llvmpipe (measured) a colour vkCmdClearAttachments
// inside a dynamic-rendering pass stops every later draw in that pass from producing a fragment. It is also the
// cheaper of the two -- no extra command, no pass exit.
bool clear_color(const float rgba[4]);

// Clear a rectangle, which is what D3D's Clear actually takes. This is the vkCmdClearAttachments form, and it
// carries the warning above: on that same driver, draws recorded *after* it in the same pass produce nothing,
// so the caller should end the frame or start a fresh one after a partial clear. Nothing in the port's own
// tests depends on that behaviour, and the D3D8 layer's Clear is documented to do the same.
bool clear_color_rect(const float rgba[4], uint32_t x, uint32_t y, uint32_t w, uint32_t h);

// Depth, cleared with vkCmdClearAttachments because this is the form that takes a rectangle and because a
// depth clear was measured *not* to stop later draws on the driver in question.
bool clear_depth(float value);

// End the pass, submit and wait. After this the recorded work has actually happened, which is what lets a
// readback -- or a PNG written from one -- mean anything.
bool end_frame();

// Copy a colour target's pixels out, row-major from the top. `out` must hold width*height*4 bytes.
//
// **In the image's own byte order, not RGBA.** For the B8G8R8A8 targets this port uses -- the game's A8R8G8B8
// -- that is B, G, R, A, because that is what the game itself reads back through LockRect: it asked for
// A8R8G8B8 and would read those bytes. Normalising here would hand the D3D8 layer a picture with two
// channels the wrong way round, which is not a detail anyone notices until a screenshot comes out orange.
// For screenshots use write_png_from_color_target() below, which does the conversion once, in one place.
bool read_color(const Image *img, void *out);

// Read a colour target back and write it as a PNG. This is the screenshot path, and it exists so that no
// caller feeds read_color's byte order straight into a writer that expects RGBA: doing that swaps red and
// blue in every image, and the only test that looked at the file checked its header rather than its pixels.
bool write_png_from_color_target(const char *path, const Image *img);

// The staging buffer draw calls upload through, sized on first use and grown in whole megabytes.
Buffer &vertex_staging();

} // namespace gfx
