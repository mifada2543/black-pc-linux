#include "gfx/gfx_vulkan.h"

#include "d3d/vertex_posix.h"

#include "gfx/png.h"

#include "platform/posix/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

namespace gfx {
namespace {

// The colour the current pass should start cleared with, when a Clear asked for one. Declared at the top of
// this namespace because begin_pass() -- the first function to read it -- is the first function that uses a
// Vulkan handle.
bool g_pass_clear = false;
float g_pass_clear_colour[4] = {0.0f, 0.0f, 0.0f, 1.0f};

VkInstance g_instance;
VkPhysicalDevice g_physical;
VkDevice g_device;
VkQueue g_queue;
uint32_t g_queue_family;
VkCommandPool g_pool;
VkCommandBuffer g_cmd;

// Whether a dynamic-rendering pass is open on g_cmd. Several commands may not be recorded while it
// is -- a buffer copy and a layout barrier among them -- and recording one anyway is undefined
// behaviour that a driver is free to answer with a crash. Everything that needs to know asks this.
bool g_pass_open = false;

// Whether the chosen device is a CPU rasteriser. There is no such thing as device-local memory on
// one, and asking for it is not merely wasteful: 32-bit llvmpipe (Mesa 25.2.8) segfaults inside
// vkCmdCopyBuffer when the destination is DEVICE_LOCAL. Measured, not inferred -- the same copy
// with a host-coherent destination runs. So buffers on a CPU device get host memory, and the
// mapping they come with turns every upload into the memcpy it would have wanted to be anyway.
bool g_device_is_cpu = false;

// The pass's attachments. Declared up here because ensure_pass(), which opens the pass, is far
// above the code that sets them.
Image *g_color;
Image *g_depth;

// Staging buffers that a recorded copy still refers to.
//
// This list exists because of a use-after-free that reads as fine as anything else. A copy is recorded into
// the frame's command buffer and the staging buffer it names is destroyed on the spot; the copy has not run
// yet, and when the frame is submitted it runs against memory the driver has been free to reuse. It works
// more often than not, which is what makes it dangerous -- the bytes come out right, right up until a reuse
// lands somewhere that matters, and then the process dies somewhere else entirely. The frame's copies are
// finished once end_frame has waited, and that is where these are released.
std::vector<Buffer> g_staging_pending;

// The one reusable index buffer. Rewritten whenever the caller has different indices: a draw that needs
// indices nearly always needs the same ones as the last draw, and allocating per draw would be the
// largest cost in a frame made entirely of quads.
Buffer g_index_buffer;
uint64_t g_index_capacity = 0;

// Whether the frame has a pipeline bound. A draw without one is recorded anyway and faults at submit,
// so the draw path asks first and skips with a reason.
bool g_pipeline_bound = false;

// A handle, not a pointer, so that the D3D layer can hold one without naming a Vulkan type (section 3.2).
std::vector<ManagedBuffer> g_managed;
bool g_recording;
AdapterInfo g_adapter;
std::string g_error;
std::vector<VkExtensionProperties> g_device_extensions;
std::vector<std::string> g_skip_reasons; // one per physical device, empty when it was accepted
std::vector<AdapterInfo> g_adapters;      // every device, in enumeration order (see adapter_at)
int g_chosen_index = -1;                   // which one the loader drew on, -1 before init
int g_index_override = -1;                 // BLACKLINUX_VK_INDEX / vulkan_set_device_index: force one
std::string g_device_override;
bool g_probe_enabled = true;
bool g_probe_read_env = false;

// ---- the window ----
//
// Everything the window path needs, in one block, because the alternative is eight file statics that have
// to agree about which queue presents. It sits with the device block above rather than down where the
// swapchain code is, because init() writes half of it long before swapchain_create reads it.

// Instance extensions the window system asked for, before the instance exists. VK_KHR_surface is added by
// init() itself rather than being asked for: every window system needs it, and a caller that had to
// remember it would be a caller whose window fails for a reason nobody can see.
std::vector<std::string> g_want_instance_extensions;
CreateSurfaceFn g_surface_creator = nullptr;
void *g_surface_user = nullptr;
VkSurfaceKHR g_surface = VK_NULL_HANDLE;

// The surface's entry points, loaded once. VK_KHR_surface is an instance extension, so its functions are
// not necessarily in the loader's exported set, and calling one that was never loaded is a jump through
// a null pointer rather than an error.
PFN_vkDestroySurfaceKHR pfnDestroySurface;
PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR pfnSurfaceCapabilities;
PFN_vkGetPhysicalDeviceSurfaceFormatsKHR pfnSurfaceFormats;
PFN_vkGetPhysicalDeviceSurfacePresentModesKHR pfnSurfacePresentModes;
PFN_vkGetPhysicalDeviceSurfaceSupportKHR pfnSurfaceSupport;
PFN_vkCreateSwapchainKHR pfnCreateSwapchain;
PFN_vkDestroySwapchainKHR pfnDestroySwapchain;
PFN_vkGetSwapchainImagesKHR pfnSwapchainImages;
PFN_vkAcquireNextImageKHR pfnAcquireNextImage;
PFN_vkQueuePresentKHR pfnQueuePresent;

// The queue family that presents, and its queue. A second family only exists on a device whose
// presentation-capable family is not its graphics family; both queues are requested at device creation,
// because that is the only moment Vulkan lets a queue be asked for at all.
uint32_t g_present_family = UINT32_MAX;
VkQueue g_present_queue = VK_NULL_HANDLE;

// The swapchain. `images` are the drawables wrapped as Images so that set_render_target() takes them
// unchanged, and `handles` are the same images again as VkImage, kept apart because an ImageView is not
// an Image. (NOTES records two places where this port mixes the two: in a 32-bit build, where every
// non-dispatchable handle is a pointer, it compiles and works, and in a 64-bit build it is wrong.)
VkSwapchainKHR g_swapchain = VK_NULL_HANDLE;
std::vector<Image> g_swapchain_images;
std::vector<VkImage> g_swapchain_handles;
uint32_t g_swapchain_width = 0, g_swapchain_height = 0;
VkFormat g_swapchain_format = VK_FORMAT_UNDEFINED;
const char *g_swapchain_present_mode = "none";
uint32_t g_swapchain_generation = 0;
bool g_swapchain_stale = false;
bool g_swapchain_wants_immediate = false; // what the caller asked for; kept so a rebuild asks again
bool g_swapchain_can_read_back = false;  // whether the drawables can be copied out of

// The two semaphores one frame in flight needs. One frame at a time is a choice, not a limitation: the
// loader renders, presents and waits before it does anything else (see end_frame), so a second frame in
// flight would buy nothing and would double the ways to use an image the presentation engine still owns.
VkSemaphore g_acquire_done = VK_NULL_HANDLE; // presentation engine -> graphics
VkSemaphore g_release_done = VK_NULL_HANDLE; // graphics -> presentation engine

const char *vk_result_name(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    default: return "a VkResult this port does not name";
    }
}

bool fail(const char *what, VkResult r)
{
    g_error = std::string(what) + ": " + vk_result_name(r);
    return false;
}

// Physical devices in preference order: a discrete GPU first, then anything else, then a software
// rasteriser. llvmpipe last is deliberate -- it works, and it is what the headless tests fall back to, but
// on a machine with a real GPU there is no reason to draw with it.
int device_score(const VkPhysicalDeviceProperties &p)
{
    switch (p.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 0;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 1;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return 3;
    default: return 4;
    }
}

// Whether the chosen device offers a device extension. Kept even though nothing needs one yet: the D3D8
// layer will (INTZ-style depth sampling is an extension on some drivers, see host.h's FMT_INTZ), and finding
// out then would mean a second round of driver archaeology.
} // namespace

bool has_device_extension(const char *name)
{
    for (const VkExtensionProperties &e : g_device_extensions)
        if (!strcmp(e.extensionName, name)) return true;
    return false;
}

namespace {

// The render pass a single attachment needs, built fresh per pass: this port has one colour attachment and
// at most one depth attachment, and the D3D8 layer changes targets mid-frame, so a cached pass object per
// attachment pair would be a cache with no hits.
VkCommandBuffer begin_pass(Image *color, Image *depth)
{
    if (color->format == VK_FORMAT_UNDEFINED || !color->view) return VK_NULL_HANDLE;
    if (g_pass_open) return g_cmd; // one pass at a time; a second Begin would be an error, not a no-op

    VkRenderingAttachmentInfo color_attach;
    memset(&color_attach, 0, sizeof color_attach);
    color_attach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color_attach.imageView = color->view;
    color_attach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    // The pass preserves what is in the attachment unless a Clear asked otherwise. A pass that always cleared
    // would be the loader deciding, silently, that every render target starts black -- and the Xbox's device
    // does not: the game issues Clear, and between a clear and the next one whatever it drew last is still
    // there. That is what a target the game reads back with LockRect expects to find.
    //
    // A Clear is expressed as this pass's loadOp rather than as vkCmdClearAttachments, and that is not a
    // style choice: on 32-bit llvmpipe (measured) a colour vkCmdClearAttachments inside a dynamic-rendering
    // pass stops every later draw in that pass from producing a fragment. loadOp is the mechanism this driver
    // handles correctly, and it is also the cheaper one -- no extra command, no extra pass exit.
    color_attach.loadOp = g_pass_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    color_attach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (g_pass_clear) memcpy(&color_attach.clearValue.color, g_pass_clear_colour, sizeof g_pass_clear_colour);
    g_pass_clear = false;

    // The depth attachment is one pointer with no count, and leaving it null when there is no depth buffer
    // is what tells Vulkan there is none -- the Xbox's device has the same optional depth surface.
    VkRenderingAttachmentInfo depth_attach;
    memset(&depth_attach, 0, sizeof depth_attach);
    depth_attach.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    if (depth && depth->view) {
        depth_attach.imageView = depth->view;
        depth_attach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth_attach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; // depth starts cleared; see clear_depth
        depth_attach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth_attach.clearValue.depthStencil = {1.0f, 0};
    }

    VkRenderingInfo info;
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = {{0, 0}, {color->width, color->height}};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &color_attach;
    if (depth && depth->view) info.pDepthAttachment = &depth_attach;

    vkCmdBeginRendering(g_cmd, &info);
    g_pass_open = true;
    return g_cmd;
}

void end_pass()
{
    if (!g_recording || !g_pass_open) return;
    vkCmdEndRendering(g_cmd);
    g_pass_open = false;
}

void barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
             VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(g_cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Images start in UNDEFINED and move to the layout their use needs. Keeping the current layout with the
// image is the whole reason this port has no render pass objects to remember it in.
//
// One table, not one per function: the slot an Image is given and the layout it is looked up in have to be
// the same table, and the first version of this had a vector in each place, so every image was handed slot
// 0 and then looked up in a vector that had never been grown.
// ---- the device probe ----
//
// A child process that creates a device for one physical device and submits one command buffer that does
// real work (a buffer fill), then reports the answer in its exit status: 0 survived, 1 refused, and a
// signal means the driver died. A crashed driver cannot be caught in-process -- there is no SEH on Linux,
// and a signal handler in the middle of a driver submit could only watch it die -- so the question is asked
// of a child and the parent is left with a number.
//
// What made this necessary, measured on 2026-10-17 with a program written from scratch rather than from the
// loader: the 32-bit Intel driver (ANV, Mesa, Intel ADL GT2, kernel 6.17) segfaults inside vkQueueSubmit for
// *any* command buffer, with a plain VkRenderPass as well as with dynamic rendering. llvmpipe submits and
// draws correctly. Picking a device by preference alone would therefore take the loader down at the first
// frame on any machine with that GPU, so the probe exists.
enum ProbeResult { PROBE_OK = 0, PROBE_REFUSED = 1 };

__attribute__((noreturn)) static void probe_exit(int code)
{
    fflush(nullptr);
    _exit(code);
}

static void probe_child(uint32_t physical_index)
{
    VkApplicationInfo app;
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof ici);
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance inst;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) probe_exit(PROBE_REFUSED);

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(inst, &count, nullptr);
    if (physical_index >= count) probe_exit(PROBE_REFUSED);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(inst, &count, devices.data());
    VkPhysicalDevice pd = devices[physical_index];

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &family_count, families.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; i++)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            family = i;
            break;
        }
    if (family == UINT32_MAX) probe_exit(PROBE_REFUSED);

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof qci);
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceFeatures feats;
    memset(&feats, 0, sizeof feats);
    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof dci);
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &feats;
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, nullptr, &dev) != VK_SUCCESS) probe_exit(PROBE_REFUSED);

    VkQueue queue;
    vkGetDeviceQueue(dev, family, 0, &queue);

    // A 256-byte buffer and a barrier. Both exist so the command buffer is not empty -- an empty submit would
    // not prove the driver can execute anything, which is the question being asked.
    //
    // It used to be a transfer: this buffer plus `vkCmdFillBuffer`. The reason to drop it stands on its own and
    // does not depend on how any driver behaves: **the game never issues a transfer command** -- it draws and
    // it writes push buffers -- so probing with one judges the driver by work this port will never ask of it.
    // A barrier is a real command, and it is one the game's own workload does contain.
    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof bci);
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = 256;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VkBuffer buffer;
    if (vkCreateBuffer(dev, &bci, nullptr, &buffer) != VK_SUCCESS) probe_exit(PROBE_REFUSED);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buffer, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    // Host-visible *and* coherent, falling back to plain host-visible. This loop used to have no `break`, so it
    // kept the LAST host-visible type rather than a coherent one -- an arbitrary heap choice. It was tested as
    // a possible cause of the Intel submit crash and turned out not to be one, but the rule is now the same one
    // create_buffer() uses (HOST_VISIBLE | HOST_COHERENT), so there is one rule for host access, not two.
    uint32_t type = UINT32_MAX;
    uint32_t fallback = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (!(req.memoryTypeBits & (1u << i))) continue;
        const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if (!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
        if (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
            type = i;
            break;
        }
        if (fallback == UINT32_MAX) fallback = i;
    }
    if (type == UINT32_MAX) type = fallback;
    if (type == UINT32_MAX) probe_exit(PROBE_REFUSED);
    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof mai);
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    VkDeviceMemory memory;
    if (vkAllocateMemory(dev, &mai, nullptr, &memory) != VK_SUCCESS) probe_exit(PROBE_REFUSED);
    vkBindBufferMemory(dev, buffer, memory, 0);

    VkCommandPoolCreateInfo pci;
    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = family;
    VkCommandPool pool;
    if (vkCreateCommandPool(dev, &pci, nullptr, &pool) != VK_SUCCESS) probe_exit(PROBE_REFUSED);
    VkCommandBufferAllocateInfo cai;
    memset(&cai, 0, sizeof cai);
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(dev, &cai, &cmd) != VK_SUCCESS) probe_exit(PROBE_REFUSED);

    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) probe_exit(PROBE_REFUSED);
    VkMemoryBarrier barrier;
    memset(&barrier, 0, sizeof barrier);
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr,
                         0, nullptr);
    // End the command buffer before submitting it. A command buffer handed to vkQueueSubmit has to be in
    // the pending or executable state (VUID-vkQueueSubmit-pCommandBuffers-00070); one that is still
    // recording is invalid usage, and this is the exact bug that made Intel's driver die inside
    // vkQueueSubmit while llvmpipe quietly tolerated it. submit_frame ends its buffer for the same reason,
    // and the comment there says why -- this path was simply missed when that was fixed.
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS) probe_exit(PROBE_REFUSED);
    VkSubmitInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) probe_exit(PROBE_REFUSED);
    if (vkQueueWaitIdle(queue) != VK_SUCCESS) probe_exit(PROBE_REFUSED);
    probe_exit(PROBE_OK);
}

VkImageLayout &layout_of(const Image &img)
{
    static std::vector<VkImageLayout> layouts;
    if (layouts.size() <= img.layout_slot) layouts.resize(img.layout_slot + 1, VK_IMAGE_LAYOUT_UNDEFINED);
    return layouts[img.layout_slot];
}

uint32_t new_layout_slot()
{
    static uint32_t next = 0;
    return next++;
}

// A command buffer of its own, for work whose answer is a return value.
//
// The frame's command buffer cannot do that job. While a pass is open the buffer is in the executable state
// and may not even be ended, let alone submitted, and a readback has no way around it: the caller wants the
// bytes when the call returns, not at the end of the frame. So it gets a buffer that is begun, recorded,
// ended, submitted, waited and freed in one go.
//
// This is not a shortcut. Recording the readback into the frame instead is the version that looks right and
// silently returns whatever the staging buffer happened to be holding -- a recycled allocation -- because a
// barrier and a vkQueueWaitIdle do not submit anything. Two of the first few checks here passed for exactly
// that reason, on memory that happened to hold the right bytes.
VkCommandBuffer one_shot_begin()
{
    if (!ready()) return VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cai;
    memset(&cai, 0, sizeof cai);
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = g_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(g_device, &cai, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
        vkFreeCommandBuffers(g_device, g_pool, 1, &cmd);
        return VK_NULL_HANDLE;
    }
    return cmd;
}

void one_shot_end(VkCommandBuffer cmd)
{
    if (cmd == VK_NULL_HANDLE) return;
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_queue);
    vkFreeCommandBuffers(g_device, g_pool, 1, &cmd);
}

VkCommandBuffer ensure_recording()
{
    if (!g_recording && !begin_frame()) return VK_NULL_HANDLE;
    return g_cmd;
}

// A frame is recording and a pass is open on it, which is what anything that draws or sets dynamic state
// needs. The pass opens here rather than at the start of the frame so that a transfer or a layout barrier
// recorded before the first draw has somewhere legal to go.
VkCommandBuffer ensure_pass()
{
    if (!ensure_recording()) return VK_NULL_HANDLE;
    if (!g_pass_open && !begin_pass(g_color, g_depth)) return VK_NULL_HANDLE;
    return g_cmd;
}

// End the pass, submit, wait, and release the frame's staging. Defined down with the frames and used by the
// swapchain above, because presenting a drawable is the same tail with two semaphones in it and two copies
// of it would be two chances to get the hand-over wrong in one of them.
bool submit_frame(VkSemaphore wait, VkSemaphore signal, Image *present_target);

} // namespace

// ---- init ----

bool ready() { return g_device != VK_NULL_HANDLE; }
const char *last_error() { return g_error.c_str(); }
const AdapterInfo &adapter() { return g_adapter; }

std::string vulkan_skip_reason(uint32_t index)
{
    return index < g_skip_reasons.size() ? g_skip_reasons[index] : std::string();
}

uint32_t vulkan_device_count() { return (uint32_t)g_skip_reasons.size(); }

uint32_t adapter_count() { return (uint32_t)g_adapters.size(); }

const AdapterInfo &adapter_at(uint32_t index)
{
    static const AdapterInfo none;
    return index < g_adapters.size() ? g_adapters[index] : none;
}

int adapter_index() { return g_chosen_index; }

void vulkan_set_device_index(int index) { g_index_override = index; }

void vulkan_set_device_override(const char *name_substring)
{
    g_device_override = name_substring ? name_substring : "";
    g_probe_read_env = false; // re-read, so an explicit call wins over what the environment said
}

void vulkan_set_probe_enabled(bool on)
{
    g_probe_enabled = on;
    g_probe_read_env = false;
}
VkQueue queue() { return g_queue; }
VkDevice device() { return g_device; }

bool init()
{
    if (ready()) return true;
    g_error.clear();

    VkApplicationInfo app;
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "blacklinux";
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof ici);
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    // No layers and no validation: the loader's graphics path has to work on a machine with nothing
    // installed but the driver, and validation output on a player's screen is not a feature.
    //
    // The window system's extensions come from the window system. Which one reaches an X11 drawable and
    // which reaches a Wayland surface is not in Vulkan's headers, and guessing is not an option: an
    // instance created without the one its surface needs is an instance whose surface cannot be made, and
    // "the window does not work" is a worse report than the extension list.
    // VK_KHR_surface is added here rather than asked for, because every window system needs it and SDL2
    // on this machine already names it (measured, X11) -- so the list is deduplicated rather than trusted.
    std::vector<const char *> instance_exts;
    if (g_surface_creator) instance_exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    for (const std::string &name : g_want_instance_extensions) {
        bool have = false;
        for (const char *e : instance_exts)
            if (name == e) have = true;
        if (!have) instance_exts.push_back(name.c_str());
    }
    ici.enabledExtensionCount = (uint32_t)instance_exts.size();
    ici.ppEnabledExtensionNames = instance_exts.empty() ? nullptr : instance_exts.data();
    VkResult r = vkCreateInstance(&ici, nullptr, &g_instance);
    if (r != VK_SUCCESS) return fail("no Vulkan instance", r);

    if (g_surface_creator) {
        // Before the device, and that order is the whole reason this call is here rather than inside
        // swapchain_create(): the queue family that can present is chosen when the logical device is
        // created, and asking which one it is needs a surface. Everything after this is geometry.
        r = g_surface_creator(g_surface_user, g_instance, &g_surface);
        if (r != VK_SUCCESS || g_surface == VK_NULL_HANDLE) {
            g_error = "the window system could not make a Vulkan surface: " + std::string(vk_result_name(r));
            vkDestroyInstance(g_instance, nullptr);
            g_instance = VK_NULL_HANDLE;
            return false;
        }
        pfnDestroySurface = (PFN_vkDestroySurfaceKHR)vkGetInstanceProcAddr(g_instance, "vkDestroySurfaceKHR");
        pfnSurfaceCapabilities = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)
            vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        pfnSurfaceFormats = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)
            vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
        pfnSurfacePresentModes = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)
            vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");
        pfnSurfaceSupport = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)
            vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
        pfnCreateSwapchain = (PFN_vkCreateSwapchainKHR)vkGetInstanceProcAddr(g_instance, "vkCreateSwapchainKHR");
        pfnDestroySwapchain = (PFN_vkDestroySwapchainKHR)vkGetInstanceProcAddr(g_instance, "vkDestroySwapchainKHR");
        pfnSwapchainImages = (PFN_vkGetSwapchainImagesKHR)vkGetInstanceProcAddr(g_instance, "vkGetSwapchainImagesKHR");
        pfnAcquireNextImage = (PFN_vkAcquireNextImageKHR)vkGetInstanceProcAddr(g_instance, "vkAcquireNextImageKHR");
        pfnQueuePresent = (PFN_vkQueuePresentKHR)vkGetInstanceProcAddr(g_instance, "vkQueuePresentKHR");
        // Named rather than tolerated: the swapchain path calls every one of these unconditionally, so
        // "the loader did not export the surface entry points" is an explanation while a null call is a
        // crash a long way from here.
        if (!pfnDestroySurface || !pfnSurfaceCapabilities || !pfnSurfaceFormats || !pfnSurfacePresentModes ||
            !pfnSurfaceSupport || !pfnCreateSwapchain || !pfnDestroySwapchain || !pfnSwapchainImages ||
            !pfnAcquireNextImage || !pfnQueuePresent) {
            g_error = "the Vulkan loader did not export the VK_KHR_surface entry points, although the "
                      "instance was created with that extension enabled";
            pfnDestroySurface(g_instance, g_surface, nullptr);
            vkDestroyInstance(g_instance, nullptr);
            g_instance = VK_NULL_HANDLE;
            g_surface = VK_NULL_HANDLE;
            return false;
        }
        logf("[gfx] Vulkan surface created through the window system\n");
    }

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(g_instance, &count, nullptr);
    if (!count) {
        g_error = "no Vulkan physical device (is an ICD installed? /usr/share/vulkan/icd.d)";
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(g_instance, &count, devices.data());

    // Every device, by name, in enumeration order. The loader draws on one of them, but "which ones were
    // there, and what is each one" is the first question anyone asks when a picture does not appear -- and it
    // is the question the A0 gate has to answer about all of them, one at a time.
    g_adapters.assign(count, AdapterInfo());
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devices[i], &p);
        AdapterInfo &a = g_adapters[i];
        a.name = p.deviceName;
        a.api_version = p.apiVersion;
        a.driver_version = p.driverVersion;
        a.vendor_id = p.vendorID;
        a.device_id = p.deviceID;
        a.device_type = (int)p.deviceType;
        a.max_image_dimension_2d = p.limits.maxImageDimension2D;
    }

    if (!g_probe_read_env) {
        // Read once. A loader that changes its mind about the driver half way through a frame is a loader
        // with two opinions, and the environment does not change under a running process anyway.
        g_probe_read_env = true;
        if (const char *name = getenv("BLACKLINUX_VK_DEVICE")) g_device_override = name;
        if (const char *idx = getenv("BLACKLINUX_VK_INDEX")) g_index_override = atoi(idx);
        if (const char *probe = getenv("BLACKLINUX_VK_PROBE")) g_probe_enabled = atoi(probe) != 0;
    }

    // In preference order, each one probed; the first that survives wins. The others are remembered with the
    // reason, so a machine whose GPU driver is unusable says so instead of merely running slowly.
    g_skip_reasons.assign(count, std::string());
    std::vector<uint32_t> order(count);
    for (uint32_t i = 0; i < count; i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](uint32_t x, uint32_t y) {
        VkPhysicalDeviceProperties px, py;
        vkGetPhysicalDeviceProperties(devices[x], &px);
        vkGetPhysicalDeviceProperties(devices[y], &py);
        const int sx = device_score(px), sy = device_score(py);
        // Same kind of device: alphabetical, so the choice does not depend on enumeration order.
        return sx != sy ? sx < sy : strcmp(px.deviceName, py.deviceName) < 0;
    });

    int chosen = -1;
    for (uint32_t index : order) {  // in preference order, each probed in a child
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devices[index], &p);

        if (g_index_override >= 0 && (uint32_t)g_index_override != index) {
            g_skip_reasons[index] = "not selected by BLACKLINUX_VK_INDEX";
            continue;
        }
        if (!g_device_override.empty() && !strstr(p.deviceName, g_device_override.c_str())) {
            g_skip_reasons[index] = "not selected by BLACKLINUX_VK_DEVICE";
            continue;
        }
        if (g_probe_enabled) {
            fflush(nullptr);
            const pid_t pid = fork();
            if (pid == 0) probe_child(index);
            int status = 0;
            const pid_t reaped = pid > 0 ? waitpid(pid, &status, 0) : -1;
            if (reaped != pid) {
                g_skip_reasons[index] = "could not be probed (fork or waitpid failed)";
                continue;
            }
            if (WIFSIGNALED(status)) {
                char why[128];
                snprintf(why, sizeof why, "its driver died on submit (signal %d)", WTERMSIG(status));
                g_skip_reasons[index] = why;
                continue;
            }
            if (!WIFEXITED(status) || WEXITSTATUS(status) != PROBE_OK) {
                g_skip_reasons[index] = "its driver refused the device, or the submit";
                continue;
            }
        }
        g_physical = devices[index];
        g_adapter.api_version = p.apiVersion;
        g_adapter.driver_version = p.driverVersion;
        g_adapter.vendor_id = p.vendorID;
        g_adapter.device_id = p.deviceID;
        g_adapter.device_type = (int)p.deviceType;
        g_device_is_cpu = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
        g_adapter.max_image_dimension_2d = p.limits.maxImageDimension2D;
        g_adapter.name = p.deviceName;
        chosen = (int)index;
        break;
    }

    g_chosen_index = chosen;

    if (chosen < 0) {
        std::string why = "no Vulkan device survived a submit:";
        for (uint32_t i = 0; i < count; i++) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(devices[i], &p);
            why += std::string("\n  ") + p.deviceName + ": " +
                   (g_skip_reasons[i].empty() ? std::string("not tried") : g_skip_reasons[i]);
        }
        g_error = why;
        if (g_surface) {
            pfnDestroySurface(g_instance, g_surface, nullptr);
            g_surface = VK_NULL_HANDLE;
        }
        vkDestroyInstance(g_instance, nullptr);
        g_instance = VK_NULL_HANDLE;
        return false;
    }
    for (uint32_t i = 0; i < count; i++)
        if (!g_skip_reasons[i].empty()) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(devices[i], &p);
            printf("gfx: Vulkan device '%s' skipped -- %s\n", p.deviceName, g_skip_reasons[i].c_str());
            fflush(stdout);
        }

    // Dynamic rendering is the whole reason for the 1.3 requirement, so it is checked before anything else
    // is built: a driver below 1.3 would otherwise fail much later with a validation message instead of here
    // with an explanation.
    if (g_adapter.api_version < VK_API_VERSION_1_3) {
        g_error = "the device reports Vulkan " + std::to_string(VK_VERSION_MAJOR(g_adapter.api_version)) + "." +
                  std::to_string(VK_VERSION_MINOR(g_adapter.api_version)) +
                  "; this port needs 1.3 for dynamic rendering";
        return false;
    }

    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(g_physical, nullptr, &ext_count, nullptr);
    g_device_extensions.resize(ext_count);
    if (ext_count)
        vkEnumerateDeviceExtensionProperties(g_physical, nullptr, &ext_count, g_device_extensions.data());

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(g_physical, &family_count, families.data());
    g_queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; i++) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            g_queue_family = i;
            break;
        }
    }
    if (g_queue_family == UINT32_MAX) {
        g_error = "no graphics queue family";
        return false;
    }

    // The family that presents, and whether this device can present at all. It is not assumed to be the
    // graphics family: Vulkan only requires that a presentation-capable family *also* support graphics, so
    // on most devices it is the same family (measured here: family 0 of 1, on both drivers) and on some it
    // is not, and a loader that assumed would have no queue to present on there.
    g_present_family = UINT32_MAX;
    g_adapter.present_supported = false;
    if (g_surface) {
        g_present_family = g_queue_family;
        VkBool32 supported = VK_FALSE;
        pfnSurfaceSupport(g_physical, g_queue_family, g_surface, &supported);
        if (supported != VK_TRUE) {
            g_present_family = UINT32_MAX;
            for (uint32_t i = 0; i < family_count; i++) {
                if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
                if (pfnSurfaceSupport(g_physical, i, g_surface, &supported) == VK_SUCCESS && supported == VK_TRUE) {
                    g_present_family = i;
                    break;
                }
            }
        }
        g_adapter.present_supported = g_present_family != UINT32_MAX;
    }

    const float priorities[2] = {1.0f, 1.0f};
    VkDeviceQueueCreateInfo qcis[2];
    memset(qcis, 0, sizeof qcis);
    qcis[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qcis[0].queueFamilyIndex = g_queue_family;
    qcis[0].queueCount = 1;
    qcis[0].pQueuePriorities = priorities;
    // Two queues only when presentation lives on another family. One queue per entry rather than two
    // entries for one family, because the priorities of two entries naming the same family must differ and
    // asking for one family twice is the kind of request that fails with a message about queue priorities.
    uint32_t queue_count = 1;
    if (g_present_family != UINT32_MAX && g_present_family != g_queue_family) {
        qcis[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qcis[1].queueFamilyIndex = g_present_family;
        qcis[1].queueCount = 1;
        qcis[1].pQueuePriorities = priorities + 1;
        queue_count = 2;
    }

    // The features this port asks for, and why. depthClamp because the Xbox clamps Z where Vulkan would
    // discard the fragment, and the game's shaders rely on that (see the nv2a_vsh notes in the reference).
    // rasterizer discard is deliberately absent: it is not a core feature, so requesting it would need an
    // extension, and nothing in the game needs it.
    // Vulkan 1.2+ features, queried through vkGetPhysicalDeviceFeatures2 and enabled through pNext. This is
    // not optional bookkeeping: vkCmdBeginRendering -- which begin_frame() records, so every frame -- is
    // dynamic rendering, and dynamic rendering is a feature that has to be switched on at device creation. A
    // device created without it makes every begin_frame call undefined behaviour. Measured on this machine:
    // llvmpipe tolerates it, and the 32-bit Intel driver segfaults inside vkQueueSubmit, which is how it was
    // found (loader/tools/vk_intel_diag.cpp case C, and the A0 gate's per-step trace).
    // dynamicRendering is a Vulkan 1.3 core feature (promoted from VK_KHR_dynamic_rendering), so it is read
    // from VkPhysicalDeviceVulkan13Features -- the 1.2 struct does not carry it.
    VkPhysicalDeviceVulkan13Features have13{};
    have13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceFeatures2 have2{};
    have2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    have2.pNext = &have13;
    vkGetPhysicalDeviceFeatures2(g_physical, &have2);
    const bool have_dynamic_rendering = have13.dynamicRendering == VK_TRUE;

    VkPhysicalDeviceFeatures have{};
    vkGetPhysicalDeviceFeatures(g_physical, &have);
    VkPhysicalDeviceFeatures features{};
    features.fillModeNonSolid = have.fillModeNonSolid;
    features.depthClamp = have.depthClamp;
    features.multiDrawIndirect = have.multiDrawIndirect;
    features.drawIndirectFirstInstance = have.drawIndirectFirstInstance;
    features.samplerAnisotropy = have.samplerAnisotropy;
    features.shaderStorageImageExtendedFormats = have.shaderStorageImageExtendedFormats;

    // The features that are switched on, chained through pNext. Only what this layer actually uses:
    // dynamic rendering and nothing else (no synchronisation2, no 1.3-only commands).
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.dynamicRendering = have_dynamic_rendering ? VK_TRUE : VK_FALSE;

    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof dci);
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = queue_count;
    dci.pQueueCreateInfos = qcis;
    dci.pEnabledFeatures = &features;
    dci.pNext = &features13;

    // VK_KHR_swapchain is the only device extension this layer enables, and it is enabled only when there
    // is a surface to use it with. Its absence is checked by name above rather than being discovered by
    // vkCreateDevice's VK_ERROR_EXTENSION_NOT_PRESENT, which says what happened and not what to do.
    std::vector<const char *> device_exts;
    if (g_surface) {
        if (!has_device_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            g_error = "this device has no " VK_KHR_SWAPCHAIN_EXTENSION_NAME
                      ", so it cannot present to the window this loader opened";
            return false;
        }
        device_exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    dci.enabledExtensionCount = (uint32_t)device_exts.size();
    dci.ppEnabledExtensionNames = device_exts.empty() ? nullptr : device_exts.data();

    r = vkCreateDevice(g_physical, &dci, nullptr, &g_device);
    if (r != VK_SUCCESS) return fail("no Vulkan device", r);

    if (!have_dynamic_rendering) {
        // Without this feature there is no vkCmdBeginRendering, so the render path cannot work at all, and
        // saying so here is much better than letting the first frame die inside a driver.
        return fail("this driver has no dynamic rendering (Vulkan 1.2 feature), and this layer draws with it",
                    r);
    }

    vkGetDeviceQueue(g_device, g_queue_family, 0, &g_queue);
    if (g_present_family != UINT32_MAX && g_present_family != g_queue_family)
        vkGetDeviceQueue(g_device, g_present_family, 0, &g_present_queue);
    else
        g_present_queue = g_queue;

    VkCommandPoolCreateInfo pci;
    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = g_queue_family;
    // Resettable, because the frame's buffer is reset and re-begun every frame rather than ended.
    //
    // Not transient, even though one-shot buffers come out of this pool (one_shot_begin). Transient is for
    // waiting on one submission with a fence while others are in flight; a one-shot here submits and then
    // calls vkQueueWaitIdle, which the spec allows either way, and on llvmpipe 1: `TRANSIENT_BIT` plus a
    // still-recording frame buffer in the same pool segfaulted inside the driver on the first submit.
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    r = vkCreateCommandPool(g_device, &pci, nullptr, &g_pool);
    if (r != VK_SUCCESS) return fail("no command pool", r);

    VkCommandBufferAllocateInfo cai;
    memset(&cai, 0, sizeof cai);
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = g_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    r = vkAllocateCommandBuffers(g_device, &cai, &g_cmd);
    if (r != VK_SUCCESS) return fail("no command buffer", r);

    return true;
}

void shutdown()
{
    if (g_device) vkDeviceWaitIdle(g_device);
    // The window objects first, and in this order: the semaphores and the views belong to the device, the
    // swapchain belongs to the surface, and the surface belongs to the instance. Tearing them down in any
    // other order hands the driver an object whose parent is already gone, which on llvmpipe is a crash
    // rather than a validation error (there are no validation layers here).
    swapchain_destroy();
    if (g_device && g_acquire_done) vkDestroySemaphore(g_device, g_acquire_done, nullptr);
    if (g_device && g_release_done) vkDestroySemaphore(g_device, g_release_done, nullptr);
    g_acquire_done = VK_NULL_HANDLE;
    g_release_done = VK_NULL_HANDLE;
    // A frame that was begun and never submitted still holds staging the driver never read from.
    for (size_t i = 0; i < g_staging_pending.size(); i++) {
        if (g_device) destroy_buffer(&g_staging_pending[i]);
    }
    g_staging_pending.clear();
    for (size_t i = 0; i < g_managed.size(); i++) {
        if (g_device) destroy_buffer(&g_managed[i].device);
    }
    g_managed.clear();
    if (g_index_buffer.buffer) destroy_buffer(&g_index_buffer);
    if (g_device && g_pool) vkDestroyCommandPool(g_device, g_pool, nullptr);
    if (g_device) vkDestroyDevice(g_device, nullptr);
    if (g_surface && pfnDestroySurface) {
        pfnDestroySurface(g_instance, g_surface, nullptr);
        g_surface = VK_NULL_HANDLE;
    }
    if (g_instance) vkDestroyInstance(g_instance, nullptr);
    g_device = VK_NULL_HANDLE;
    g_instance = VK_NULL_HANDLE;
    g_physical = VK_NULL_HANDLE;
    g_queue = VK_NULL_HANDLE;
    g_present_queue = VK_NULL_HANDLE;
    g_pool = VK_NULL_HANDLE;
    g_cmd = VK_NULL_HANDLE;
    g_present_family = UINT32_MAX;
    g_recording = false;
    g_adapter = AdapterInfo();
}

uint32_t max_vertex_attributes()
{
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(g_physical, &p);
    return p.limits.maxVertexInputAttributes;
}

// ---- format ----

VkFormat srgb_format(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    default: return f;
    }
}

// ---- memory ----

uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid)
{
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(g_physical, &mp);
    uint32_t fallback = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (!(type_bits & (1u << i))) continue;
        const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if ((f & want) != want) continue;
        if (avoid && (f & avoid)) {
            if (fallback == UINT32_MAX) fallback = i;
            continue;
        }
        return i;
    }
    return fallback; // only a type that also has something we said to avoid
}

// ---- buffers ----

bool create_buffer(Buffer *b, VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible)
{
    if (!ready()) {
        g_error = "gfx::init has not run";
        return false;
    }
    if (size == 0) size = 4;
    // See g_device_is_cpu. A request for device-local memory on a CPU rasteriser is honoured as a request for
    // host memory, because that is what the memory is and because the alternative crashes the driver.
    if (g_device_is_cpu) host_visible = true;
    b->size = size;

    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof bci);
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = vkCreateBuffer(g_device, &bci, nullptr, &b->buffer);
    if (r != VK_SUCCESS) return fail("vkCreateBuffer", r);

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g_device, b->buffer, &req);

    const VkMemoryPropertyFlags want = host_visible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                                     : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    const uint32_t type = find_memory_type(req.memoryTypeBits, want);
    if (type == UINT32_MAX) {
        vkDestroyBuffer(g_device, b->buffer, nullptr);
        b->buffer = VK_NULL_HANDLE;
        g_error = "no memory type for a buffer";
        return false;
    }

    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof mai);
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    r = vkAllocateMemory(g_device, &mai, nullptr, &b->memory);
    if (r != VK_SUCCESS) {
        vkDestroyBuffer(g_device, b->buffer, nullptr);
        b->buffer = VK_NULL_HANDLE;
        return fail("vkAllocateMemory (buffer)", r);
    }
    vkBindBufferMemory(g_device, b->buffer, b->memory, 0);

    if (host_visible) {
        // Persistently mapped, because every draw's vertices come from here (DrawPrimitiveUP) and mapping
        // per draw would be the single biggest cost in the frame.
        r = vkMapMemory(g_device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->mapped);
        if (r != VK_SUCCESS) {
            vkDestroyBuffer(g_device, b->buffer, nullptr);
            vkFreeMemory(g_device, b->memory, nullptr);
            // Separate statements, deliberately: a chain (b->buffer = b->memory = VK_NULL_HANDLE) hands
            // VkDeviceMemory's value to a VkBuffer slot. It happens to work in this i386 build because every
            // non-dispatchable handle is a plain pointer here, but it does not compile in a 64-bit build,
            // where the two handle types are distinct.
            b->buffer = VK_NULL_HANDLE;
            b->memory = VK_NULL_HANDLE;
            b->mapped = nullptr;
            return fail("vkMapMemory (buffer)", r);
        }
    }
    return true;
}

void destroy_buffer(Buffer *b)
{
    if (!b || !g_device || !b->buffer) return;
    if (b->mapped) vkUnmapMemory(g_device, b->memory);
    vkDestroyBuffer(g_device, b->buffer, nullptr);
    if (b->memory) vkFreeMemory(g_device, b->memory, nullptr);
    *b = Buffer();
}

bool upload_buffer(Buffer *b, const void *data, VkDeviceSize size)
{
    if (!b || !b->buffer || !data || size == 0) return false;
    if (b->mapped) { // host-visible: the copy is the memcpy
        memcpy(b->mapped, data, (size_t)size);
        return true;
    }

    Buffer staging;
    if (!create_buffer(&staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) return false;
    memcpy(staging.mapped, data, (size_t)size);

    VkCommandBuffer cmd = ensure_recording();
    if (!cmd) {
        destroy_buffer(&staging);
        return false;
    }
    VkBufferCopy copy{0, 0, size};
    vkCmdCopyBuffer(cmd, staging.buffer, b->buffer, 1, &copy);
    VkBufferMemoryBarrier bar;
    memset(&bar, 0, sizeof bar);
    bar.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.buffer = b->buffer;
    bar.offset = 0;
    bar.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 1, &bar, 0, nullptr);
    // Not destroyed here: the copy naming it has not run yet. See g_staging_pending.
    g_staging_pending.push_back(staging);
    return true;
}

Buffer &vertex_staging()
{
    static Buffer staging;
    if (staging.buffer) return staging;
    // Sized for a full frame's worth of loose vertices in one go: 4 MiB is 262144 FLOAT4 vertices, which
    // is more than any frame of this game has needed. Grown in whole megabytes after that.
    create_buffer(&staging, 4u << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
    return staging;
}

// ---- images ----

namespace {

bool create_image(Image *img, uint32_t width, uint32_t height, VkFormat format, uint32_t mip_levels,
                  VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    if (!ready()) {
        g_error = "gfx::init has not run";
        return false;
    }
    if (!width || !height) return false;

    img->width = width;
    img->height = height;
    img->format = format;
    img->mip_levels = mip_levels ? mip_levels : 1;
    img->layout_slot = new_layout_slot();

    VkImageCreateInfo ici;
    memset(&ici, 0, sizeof ici);
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {width, height, 1};
    ici.mipLevels = img->mip_levels;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = vkCreateImage(g_device, &ici, nullptr, &img->image);
    if (r != VK_SUCCESS) return fail("vkCreateImage", r);

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g_device, img->image, &req);
    const uint32_t type = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof mai);
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type == UINT32_MAX ? find_memory_type(req.memoryTypeBits, 0) : type;
    r = vkAllocateMemory(g_device, &mai, nullptr, &img->memory);
    if (r != VK_SUCCESS) {
        vkDestroyImage(g_device, img->image, nullptr);
        img->image = VK_NULL_HANDLE;
        return fail("vkAllocateMemory (image)", r);
    }
    vkBindImageMemory(g_device, img->image, img->memory, 0);

    VkImageViewCreateInfo vci;
    memset(&vci, 0, sizeof vci);
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = img->image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {aspect, 0, img->mip_levels, 0, 1};
    r = vkCreateImageView(g_device, &vci, nullptr, &img->view);
    if (r != VK_SUCCESS) {
        vkDestroyImage(g_device, img->image, nullptr);
        vkFreeMemory(g_device, img->memory, nullptr);
        // Separate statements, not a chain -- see the note in create_buffer().
        img->image = VK_NULL_HANDLE;
        img->memory = VK_NULL_HANDLE;
        img->view = VK_NULL_HANDLE;
        return fail("vkCreateImageView", r);
    }
    layout_of(*img) = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

} // namespace

bool create_color_target(Image *img, uint32_t width, uint32_t height, VkFormat format, bool linear)
{
    const VkFormat f = linear ? srgb_format(format) : format;
    return create_image(img, width, height, f, 1,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT);
}

bool create_depth_target(Image *img, uint32_t width, uint32_t height, VkFormat format)
{
    return create_image(img, width, height, format, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                        VK_IMAGE_ASPECT_DEPTH_BIT);
}

bool create_texture(Image *img, uint32_t width, uint32_t height, VkFormat format, uint32_t mip_levels,
                    bool sampled)
{
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (sampled) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    return create_image(img, width, height, format, mip_levels, usage, VK_IMAGE_ASPECT_COLOR_BIT);
}

void destroy_image(Image *img)
{
    if (!img || !g_device || !img->image) return;
    if (img->view) vkDestroyImageView(g_device, img->view, nullptr);
    vkDestroyImage(g_device, img->image, nullptr);
    if (img->memory) vkFreeMemory(g_device, img->memory, nullptr);
    *img = Image();
}

bool upload_texture(Image *img, const void *pixels, VkDeviceSize bytes, uint32_t level)
{
    if (!img || !img->image || !pixels || bytes == 0) return false;

    // A staging buffer the size of one mip level, reused through the frame: the game's textures are
    // re-uploaded whenever it locks them, and a fresh allocation per upload is what makes that path slow.
    static Buffer staging;
    if (!staging.buffer || staging.size < bytes) {
        destroy_buffer(&staging);
        if (!create_buffer(&staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) return false;
    }
    memcpy(staging.mapped, pixels, (size_t)bytes);

    VkCommandBuffer cmd = ensure_recording();
    if (!cmd) return false;

    if (layout_of(*img) != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        VkImageMemoryBarrier b;
        memset(&b, 0, sizeof b);
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = layout_of(*img);
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img->image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        layout_of(*img) = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }

    VkBufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
    copy.imageExtent = {img->width, img->height, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    // Straight back to what it was: a texture the game just locked is about to be sampled, and the D3D8
    // layer does not track layouts of its own.
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img->image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
    layout_of(*img) = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

// ---- samplers and texture bindings ----

namespace {

VkSamplerAddressMode to_vk_address(AddressMode m)
{
    switch (m) {
    case ADDRESS_WRAP: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case ADDRESS_MIRROR: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case ADDRESS_CLAMP: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    // Mirror-clamp-to-edge is Vulkan's name for the mode the Xbox calls mirror-once, and it is core since
    // 1.3. It differs from mirrored repeat only for the 0.5 texture coordinate.
    case ADDRESS_MIRROR_ONCE: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case ADDRESS_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

VkFilter to_vk_filter(FilterMode f)
{
    return f == FILTER_POINT ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

VkSamplerMipmapMode to_vk_mipmap(FilterMode f)
{
    return f == FILTER_POINT ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
}

} // namespace

bool create_sampler(Sampler *s, const SamplerDesc &desc)
{
    if (!ready()) {
        g_error = "gfx::init has not run";
        return false;
    }
    VkSamplerCreateInfo sci;
    memset(&sci, 0, sizeof sci);
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = to_vk_filter(desc.mag_filter);
    sci.minFilter = to_vk_filter(desc.min_filter);
    sci.mipmapMode = to_vk_mipmap(desc.mip_filter);
    sci.addressModeU = to_vk_address(desc.u);
    sci.addressModeV = to_vk_address(desc.v);
    sci.addressModeW = to_vk_address(desc.w);
    sci.mipLodBias = desc.lod_bias;
    sci.anisotropyEnable = desc.max_anisotropy > 1.0f ? VK_TRUE : VK_FALSE;
    sci.maxAnisotropy = desc.max_anisotropy > 1.0f ? desc.max_anisotropy : 1.0f;
    // A border colour has to be one of the named constants on a 1.3 device; the game's own border colours are
    // black or transparent black in every case the reference records, so TRANSPARENT_BLACK is the one.
    sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    sci.compareEnable = VK_FALSE;
    sci.minLod = 0.0f;
    // maxLod 0 would clamp to the coarsest mip, so "as many as there are" is a large number instead.
    sci.maxLod = desc.max_lod > 0 ? (float)desc.max_lod : 64.0f;
    const VkResult r = vkCreateSampler(g_device, &sci, nullptr, &s->sampler);
    if (r != VK_SUCCESS) return fail("vkCreateSampler", r);
    return true;
}

void destroy_sampler(Sampler *s)
{
    if (!s || !g_device || !s->sampler) return;
    vkDestroySampler(g_device, s->sampler, nullptr);
    *s = Sampler();
}

namespace {

// The placeholder pair: a 1x1 white texture and a sampler for it, created once on first use.
//
// This is not tidiness. A Vulkan descriptor that has never been written is undefined behaviour, and the
// 32-bit llvmpipe on this machine turns that into a crash inside the draw rather than into a validation
// message -- so a shader sampling a register the game never set would take the game down. The Xbox leaves
// unused samplers pointing at a default too; what that default is, only the picture can say.
struct Placeholder {
    Image image;
    Sampler sampler;
};

Placeholder &placeholder()
{
    static Placeholder p;
    if (p.image.image) return p;
    static const uint8_t white[4] = {0xFF, 0xFF, 0xFF, 0xFF}; // B8G8R8A8 white
    create_texture(&p.image, 1, 1, VK_FORMAT_B8G8R8A8_UNORM, 1, true);
    upload_texture(&p.image, white, sizeof white);
    const SamplerDesc d;
    create_sampler(&p.sampler, d);
    return p;
}

} // namespace

bool bind_textures(Pipeline *p, const TextureSlot slots[], uint32_t count)
{
    if (!p || !p->descriptor_pool || !p->set0) {
        g_error = "the pipeline has no descriptor set to bind";
        return false;
    }

    // The placeholder is made *before* the frame starts. Creating it involves a barrier to move a texture into
    // shader-read layout, and a barrier that changes an image's layout does not belong inside a render pass.
    Placeholder &ph = placeholder();
    // A pass the game already started drawing into is closed first, rather than the barrier being recorded
    // into it anyway. Closing costs a pass boundary and the next draw opens a new one; the alternative is a
    // command the specification does not allow, which a driver may answer either by ignoring the barrier --
    // leaving the texture in the wrong layout -- or by crashing.
    if (g_pass_open) end_pass();
    if (!ensure_recording()) return false;

    // The pool has room for one set, and the previous bind's set is freed rather than leaked: the game
    // rebinds textures constantly, and a pool that grows per bind is a pool that runs out mid-frame.
    if (p->bound_set) {
        vkFreeDescriptorSets(g_device, p->descriptor_pool, 1, &p->bound_set);
        p->bound_set = VK_NULL_HANDLE;
    }

    VkDescriptorSetAllocateInfo dai;
    memset(&dai, 0, sizeof dai);
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = p->descriptor_pool;
    dai.descriptorSetCount = 1;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(g_device, &dai, &set) != VK_SUCCESS) return fail("vkAllocateDescriptorSets", VK_ERROR_OUT_OF_HOST_MEMORY);
    p->bound_set = set;

    // 2D textures in 0..3, cube textures in 4..7, everything else the placeholder. One write for all eight,
    // so there is no code path that can leave a slot unwritten.
    VkDescriptorImageInfo infos[8];
    VkWriteDescriptorSet writes[8];
    memset(writes, 0, sizeof writes);
    for (uint32_t slot = 0; slot < MAX_TEXTURE_SLOTS * 2; slot++) {
        const bool cube = slot >= CUBE_SLOT_BASE;
        const uint32_t index = cube ? slot - CUBE_SLOT_BASE : slot;
        const TextureSlot *ts = index < count && slots ? &slots[index] : nullptr;
        const Image *img = (ts && ts->image && ts->cube == cube) ? ts->image : &ph.image;
        const Sampler *smp = (ts && ts->sampler && ts->cube == cube) ? ts->sampler : &ph.sampler;
        infos[slot].sampler = smp->sampler;
        infos[slot].imageView = img->view;
        infos[slot].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[slot].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[slot].dstSet = set;
        writes[slot].dstBinding = slot;
        writes[slot].descriptorCount = 1;
        writes[slot].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[slot].pImageInfo = &infos[slot];
    }
    vkUpdateDescriptorSets(g_device, MAX_TEXTURE_SLOTS * 2, writes, 0, nullptr);

    VkDescriptorSetLayout layout = p->set0;
    vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 1, &set, 0, nullptr);
    (void)layout;
    return true;
}

// ---- pipeline ----

// The codes are xd3d::VertexFormatCode from d3d/vertex_posix.h, included here so the two sides are checked
// against each other rather than kept in step by hand. The dependency runs gfx -> d3d, never the reverse:
// d3d/ names these codes as plain integers and never sees a VkFormat, which is the rule in section 3.2.
static_assert((uint32_t)xd3d::VF_UNPACKED == 0u, "the vertex-format codes have been renumbered");
static_assert((uint32_t)xd3d::VF_NONE == 19u, "gfx_format_for_vertex_code and VertexFormatCode have drifted apart");

// Upload a range starting at `offset`. Split out from upload_buffer because the two want different things
// from the caller and one of them is the common case: a fresh upload replaces everything, while a managed
// buffer's flush replaces only what the game touched since the last one.
bool update_buffer(Buffer *b, VkDeviceSize offset, VkDeviceSize size, const void *data)
{
    if (!b || !b->buffer || !data || size == 0) return false;
    if (offset + size > b->size) return false; // a range past the end is a caller bug, not a short write

    if (b->mapped) {
        memcpy((uint8_t *)b->mapped + offset, data, (size_t)size);
        return true;
    }

    Buffer staging;
    if (!create_buffer(&staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) return false;
    memcpy(staging.mapped, data, (size_t)size);

    VkCommandBuffer cmd = ensure_recording();
    if (!cmd) {
        destroy_buffer(&staging);
        return false;
    }
    VkBufferCopy copy{0, offset, size};
    vkCmdCopyBuffer(cmd, staging.buffer, b->buffer, 1, &copy);
    VkBufferMemoryBarrier bar;
    memset(&bar, 0, sizeof bar);
    bar.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.buffer = b->buffer;
    bar.offset = offset;
    bar.size = size;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 1, &bar, 0, nullptr);
    // Not destroyed here: the copy naming it has not run yet. See g_staging_pending.
    g_staging_pending.push_back(staging);
    return true;
}

bool download_buffer(const Buffer *b, VkDeviceSize offset, VkDeviceSize size, void *out)
{
    if (!b || !b->buffer || !out || size == 0) return false;
    if (offset + size > b->size) return false;

    // A host-visible buffer is its own staging, so the answer is a memcpy and no command at all.
    if (b->mapped) {
        memcpy(out, (const uint8_t *)b->mapped + offset, (size_t)size);
        return true;
    }

    Buffer staging;
    if (!create_buffer(&staging, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return false;
    // Its own command buffer, submitted and waited here: the answer is a return value, and the frame's
    // buffer cannot be submitted while a pass is open. See one_shot_begin.
    VkCommandBuffer cmd = one_shot_begin();
    if (cmd == VK_NULL_HANDLE) {
        destroy_buffer(&staging);
        return false;
    }
    VkBufferCopy copy{offset, 0, size};
    vkCmdCopyBuffer(cmd, b->buffer, staging.buffer, 1, &copy);
    VkBufferMemoryBarrier bar;
    memset(&bar, 0, sizeof bar);
    bar.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bar.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.buffer = b->buffer;
    bar.offset = offset;
    bar.size = size;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bar, 0,
                         nullptr);
    one_shot_end(cmd);
    memcpy(out, staging.mapped, (size_t)size);
    destroy_buffer(&staging);
    return true;
}

// ---- managed buffers ----


uint32_t managed_buffer_create(void *host, uint32_t bytes, VkBufferUsageFlags usage, const char **err)
{
    if (err) *err = nullptr;
    if (!host || bytes == 0) {
        if (err) *err = "a managed buffer needs memory and a size";
        return 0;
    }
    // Transfer destination is added rather than required: the caller asks for vertex or index usage, and the
    // copy across needs this one as well. Asking the caller to remember that would be asking for a bug.
    ManagedBuffer m;
    m.host = host;
    m.bytes = bytes;
    if (!create_buffer(&m.device, bytes, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false)) {
        if (err) *err = last_error();
        return 0;
    }
    // Everything is dirty at first: the game has been writing this memory since before it existed here.
    m.dirty = true;
    m.dirty_lo = 0;
    m.dirty_hi = bytes;
    g_managed.push_back(m);
    return (uint32_t)g_managed.size(); // handles are one-based, so zero can mean "none"
}

ManagedBuffer *managed_buffer_at(uint32_t handle)
{
    if (handle == 0 || handle > g_managed.size()) return nullptr;
    return &g_managed[handle - 1];
}

size_t managed_buffer_count() { return g_managed.size(); }

void managed_buffer_mark_dirty(uint32_t handle, uint64_t lo, uint64_t hi)
{
    ManagedBuffer *m = managed_buffer_at(handle);
    if (!m) return;
    if (hi > m->bytes) hi = m->bytes; // a range past the end is clamped, not copied out of bounds
    if (lo >= hi) return;
    // Accumulate rather than replace. Two writes to opposite ends of a buffer have to produce one copy
    // spanning both; keeping only the newer one would lose the earlier write without saying so.
    // Whether to accumulate or to start over is decided by a flag and not by the numbers, because a clean
    // buffer has to be told apart from a dirty one whose range happens to be empty. Spelling the clean
    // state as 0,0 and then widening it with a new mark gives [0, hi) -- a full copy from the start of the
    // buffer for what was a 64-byte write in the middle, which is the exact cost this is here to avoid.
    if (m->dirty) {
        if (lo < m->dirty_lo) m->dirty_lo = lo;
        if (hi > m->dirty_hi) m->dirty_hi = hi;
    } else {
        m->dirty_lo = lo;
        m->dirty_hi = hi;
        m->dirty = true;
    }
    m->ever_uploaded = false;
}

bool managed_buffer_flush(uint32_t handle, const char **err)
{
    ManagedBuffer *m = managed_buffer_at(handle);
    if (err) *err = nullptr;
    if (!m) {
        if (err) *err = "no such managed buffer";
        return false;
    }
    if (!m->dirty) return true; // nothing changed since the last flush, which is the common case

    const uint64_t lo = m->dirty_lo, hi = m->dirty_hi;

    const bool ok = update_buffer(&m->device, lo, hi - lo, (const uint8_t *)m->host + lo);
    if (!ok) {
        // The range is deliberately *not* cleared: a failed copy has to be retried, and forgetting it here
        // would leave the device buffer stale with nothing to say so.
        if (err) *err = last_error();
        return false;
    }
    m->dirty = false;
    m->dirty_lo = m->dirty_hi = 0;
    m->ever_uploaded = true;
    return true;
}

bool managed_buffer_bind(uint32_t handle, uint32_t binding, VkDeviceSize offset)
{
    ManagedBuffer *m = managed_buffer_at(handle);
    if (!m) return false;
    return bind_vertex_buffer(&m->device, binding, offset);
}

void managed_buffer_destroy(uint32_t handle)
{
    ManagedBuffer *m = managed_buffer_at(handle);
    if (!m) return;
    destroy_buffer(&m->device);
    m->host = nullptr;
    m->bytes = 0;
}

VkFormat gfx_format_for_vertex_code(uint32_t code)
{
    switch (code) {
    case xd3d::VF_R32_SFLOAT: return VK_FORMAT_R32_SFLOAT;
    case xd3d::VF_R32G32_SFLOAT: return VK_FORMAT_R32G32_SFLOAT;
    case xd3d::VF_R32G32B32_SFLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
    case xd3d::VF_R32G32B32A32_SFLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case xd3d::VF_R16G16_SFLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case xd3d::VF_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case xd3d::VF_R16_SINT: return VK_FORMAT_R16_SINT;
    case xd3d::VF_R16G16_SINT: return VK_FORMAT_R16G16_SINT;
    case xd3d::VF_R16G16B16_SINT: return VK_FORMAT_R16G16B16_SINT;
    case xd3d::VF_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
    case xd3d::VF_R16_SNORM: return VK_FORMAT_R16_SNORM;
    case xd3d::VF_R16G16_SNORM: return VK_FORMAT_R16G16_SNORM;
    case xd3d::VF_R16G16B16_SNORM: return VK_FORMAT_R16G16B16_SNORM;
    case xd3d::VF_R16G16B16A16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case xd3d::VF_R8_SNORM: return VK_FORMAT_R8_SNORM;
    case xd3d::VF_R8G8_SNORM: return VK_FORMAT_R8G8_SNORM;
    case xd3d::VF_R8G8B8_SNORM: return VK_FORMAT_R8G8B8_SNORM;
    case xd3d::VF_R8G8B8A8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    default:
        // VF_UNPACKED lands here along with VF_NONE, and both are refused for the same reason: there is no
        // honest format to hand back. The caller unpacks it, or reads nothing.
        return VK_FORMAT_UNDEFINED;
    }
}


uint32_t g_bound_binding = 0;
bool create_pipeline(Pipeline *p, const PipelineDesc &desc)
{
    if (!ready()) {
        g_error = "gfx::init has not run";
        return false;
    }
    if (!desc.vertex_spirv || !desc.fragment_spirv) return false;

    auto make_module = [&](const void *code, size_t bytes, VkShaderModule *out) {
        VkShaderModuleCreateInfo sci;
        memset(&sci, 0, sizeof sci);
        sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        sci.codeSize = bytes;
        sci.pCode = (const uint32_t *)code; // the API takes a word pointer; the bytes behind it are the file's
        return vkCreateShaderModule(g_device, &sci, nullptr, out) == VK_SUCCESS;
    };
    if (!make_module(desc.vertex_spirv, desc.vertex_spirv_size, &p->vertex_module)) return fail("vertex shader module", VK_ERROR_INITIALIZATION_FAILED);
    if (!make_module(desc.fragment_spirv, desc.fragment_spirv_size, &p->fragment_module)) return fail("fragment shader module", VK_ERROR_INITIALIZATION_FAILED);

    // Set 0 is one combined image sampler per binding. The game samples a 2D texture and a cube texture
    // from the same shader register (nv2a_psh.cpp:92-93) and Vulkan forbids one descriptor for two image
    // types, so the D3D8 layer gives each its own binding here rather than sharing one.
    VkDescriptorSetLayoutBinding sb{};
    sb.binding = 0;
    sb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sb.descriptorCount = 8;
    sb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo slci;
    memset(&slci, 0, sizeof slci);
    slci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    slci.bindingCount = 1;
    slci.pBindings = &sb;
    if (vkCreateDescriptorSetLayout(g_device, &slci, nullptr, &p->set0) != VK_SUCCESS)
        return fail("descriptor set layout", VK_ERROR_INITIALIZATION_FAILED);

    VkPipelineLayoutCreateInfo plci;
    memset(&plci, 0, sizeof plci);
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &p->set0;
    if (vkCreatePipelineLayout(g_device, &plci, nullptr, &p->layout) != VK_SUCCESS)
        return fail("pipeline layout", VK_ERROR_INITIALIZATION_FAILED);

    std::vector<VkPipelineShaderStageCreateInfo> stages(2);
    memset(stages.data(), 0, sizeof stages[0] * stages.size());
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = p->vertex_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = p->fragment_module;
    stages[1].pName = "main";

    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attrs;
    // The bindings come from the caller's list, because the stride is the caller's to know: the game declares
    // a 16-FLOAT4 vertex and its attributes may cover only part of it. Deriving a stride from the attributes
    // instead would silently mis-stride every vertex whose declaration has a gap.
    bindings.reserve(desc.binding_count);
    for (uint32_t i = 0; i < desc.binding_count; i++) {
        VkVertexInputBindingDescription v;
        memset(&v, 0, sizeof v);
        v.binding = desc.bindings[i].binding;
        v.stride = desc.bindings[i].stride;
        v.inputRate = desc.bindings[i].rate == 1 ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
        bindings.push_back(v);
    }
    for (uint32_t i = 0; i < desc.attribute_count; i++) {
        VkVertexInputAttributeDescription v;
        memset(&v, 0, sizeof v);
        v.location = desc.attributes[i].location;
        v.binding = desc.attributes[i].binding;
        // The one place a vertex format code becomes a Vulkan format. A code with no Vulkan equivalent --
        // NORMPACKED3, VF_UNPACKED -- resolves to VK_FORMAT_UNDEFINED, which vkCreateGraphicsPipelines
        // refuses with a named error. That is the outcome wanted: the shader has to unpack it, and a
        // pipeline that quietly read four components where the console had three would draw wrong geometry.
        v.format = gfx_format_for_vertex_code(desc.attributes[i].format);
        v.offset = desc.attributes[i].offset;
        attrs.push_back(v);
    }

    VkPipelineVertexInputStateCreateInfo vi;
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = (uint32_t)bindings.size();
    vi.pVertexBindingDescriptions = bindings.empty() ? nullptr : bindings.data();
    vi.vertexAttributeDescriptionCount = (uint32_t)attrs.size();
    vi.pVertexAttributeDescriptions = attrs.empty() ? nullptr : attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia;
    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{0, 0, 1, 1, 0, 1};
    VkRect2D scissor{{0, 0}, {1, 1}};
    VkPipelineViewportStateCreateInfo vp;
    memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.pViewports = &viewport;
    vp.scissorCount = 1;
    vp.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rs;
    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = (desc.flags & PIPE_CULL_BACK) ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    rs.depthClampEnable = VK_TRUE; // the Xbox clamps depth rather than dropping the pixel (see nv2a_vsh)

    const bool has_depth = desc.depth_format != VK_FORMAT_UNDEFINED;
    VkPipelineDepthStencilStateCreateInfo ds;
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = (desc.flags & PIPE_DEPTH_TEST) ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = (desc.flags & PIPE_DEPTH_WRITE) ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    // The loader has the shader's set 0 at this point, and it is in the layout: a pipeline whose layout does
    // not declare the set its shader uses is undefined behaviour, and this llvmpipe build (32-bit, measured)
    // dereferences the missing layout and dies inside vkCreateGraphicsPipelines. Said here because the
    // loader above sets it -- but a bisect harness with the same shader and no set 0 crashes the same way,
    // which is the kind of thing worth not rediscovering.
    if (!p->set0 && desc.fragment_spirv)
        logf("gfx: the pipeline layout has no descriptor set for the fragment shader");

    VkPipelineColorBlendAttachmentState blend_state{};
    blend_state.blendEnable = (desc.flags & PIPE_BLEND) ? VK_TRUE : VK_FALSE;
    blend_state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_state.colorBlendOp = VK_BLEND_OP_ADD;
    blend_state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend_state.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                                  VK_COLOR_COMPONENT_A_BIT;

    // Multisample state is not optional with rasterization left on: a pipeline created without it is a
    // validation error, and llvmpipe (measured, 32-bit) dereferences the null pointer instead. It is also
    // where alpha-to-coverage lives, which the Xbox's alpha-tested foliage needs.
    VkPipelineMultisampleStateCreateInfo ms;
    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = (VkSampleCountFlagBits)(desc.sample_count ? desc.sample_count : 1);
    ms.sampleShadingEnable = VK_FALSE;
    ms.alphaToCoverageEnable = (desc.flags & PIPE_ALPHA_TO_COVERAGE) ? VK_TRUE : VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb;
    memset(&cb, 0, sizeof cb);
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &blend_state;

    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn;
    memset(&dyn, 0, sizeof dyn);
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = has_depth ? 2 : 2;
    dyn.pDynamicStates = dynamic_states;

    VkPipelineRenderingCreateInfo rendering;
    memset(&rendering, 0, sizeof rendering);
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &desc.color_format;
    if (has_depth) {
        rendering.depthAttachmentFormat = desc.depth_format;
    }

    VkGraphicsPipelineCreateInfo gp;
    memset(&gp, 0, sizeof gp);
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = stages.data();
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pDepthStencilState = has_depth ? &ds : nullptr;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = p->layout;
    gp.renderPass = VK_NULL_HANDLE;
    gp.pNext = &rendering;

    VkResult r = vkCreateGraphicsPipelines(g_device, VK_NULL_HANDLE, 1, &gp, nullptr, &p->pipeline);
    if (r != VK_SUCCESS) return fail("vkCreateGraphicsPipelines", r);

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
    VkDescriptorPoolCreateInfo dpci;
    memset(&dpci, 0, sizeof dpci);
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    vkCreateDescriptorPool(g_device, &dpci, nullptr, &p->descriptor_pool);
    return true;
}

// ---- d3d-layer pipeline creation (keeps VkFormat out of d3d layer) ----
// color_format/depth_format are gfx::VertexFormatCode (0 = no attachment).
// This function converts the d3d format codes to VkFormat and calls create_pipeline.
bool create_pipeline_d3d(Pipeline *p,
                         const void *vertex_spirv, size_t vertex_spirv_size,
                         const void *fragment_spirv, size_t fragment_spirv_size,
                         uint32_t color_format, uint32_t depth_format,
                         const VertexAttribute *attributes, uint32_t attribute_count,
                         const VertexBinding *bindings, uint32_t binding_count,
                         uint32_t flags, uint32_t sample_count)
{
    gfx::PipelineDesc desc = {};
    desc.vertex_spirv = vertex_spirv;
    desc.vertex_spirv_size = vertex_spirv_size;
    desc.fragment_spirv = fragment_spirv;
    desc.fragment_spirv_size = fragment_spirv_size;
    desc.color_format = gfx_format_for_vertex_code(color_format);
    desc.depth_format = depth_format ? gfx_format_for_vertex_code(depth_format) : VK_FORMAT_UNDEFINED;
    desc.attributes = attributes;
    desc.attribute_count = attribute_count;
    desc.bindings = bindings;
    desc.binding_count = binding_count;
    desc.flags = flags;
    desc.sample_count = sample_count;
    return create_pipeline(p, desc);
}

void destroy_pipeline(Pipeline *p)
{
    if (!p || !g_device) return;
    p->bound_set = VK_NULL_HANDLE; // freed with the pool below
    if (p->descriptor_pool) vkDestroyDescriptorPool(g_device, p->descriptor_pool, nullptr);
    if (p->pipeline) vkDestroyPipeline(g_device, p->pipeline, nullptr);
    if (p->layout) vkDestroyPipelineLayout(g_device, p->layout, nullptr);
    if (p->set0) vkDestroyDescriptorSetLayout(g_device, p->set0, nullptr);
    if (p->fragment_module) vkDestroyShaderModule(g_device, p->fragment_module, nullptr);
    if (p->vertex_module) vkDestroyShaderModule(g_device, p->vertex_module, nullptr);
    *p = Pipeline();
}

// ---- the window: surface and swapchain ----

void vulkan_add_instance_extension(const char *name)
{
    if (!name || !*name) return;
    for (const std::string &have : g_want_instance_extensions)
        if (have == name) return; // asked for twice is not an error, but it does not need saying twice
    g_want_instance_extensions.push_back(name);
}

void vulkan_set_surface_creator(CreateSurfaceFn fn, void *user)
{
    g_surface_creator = fn;
    g_surface_user = user;
}

bool surface_ready() { return g_surface != VK_NULL_HANDLE; }

bool swapchain_alive() { return g_swapchain != VK_NULL_HANDLE; }
uint32_t swapchain_width() { return g_swapchain_width; }
uint32_t swapchain_height() { return g_swapchain_height; }
uint32_t swapchain_image_count() { return (uint32_t)g_swapchain_images.size(); }
VkFormat swapchain_format() { return g_swapchain_format; }
const char *swapchain_present_mode_name() { return g_swapchain_present_mode; }
uint32_t swapchain_generation() { return g_swapchain_generation; }
bool swapchain_out_of_date() { return g_swapchain_stale; }
bool swapchain_can_read_back() { return g_swapchain_can_read_back; }

Image *swapchain_image(uint32_t index)
{
    return index < g_swapchain_images.size() ? &g_swapchain_images[index] : nullptr;
}

namespace {

const char *present_mode_name(VkPresentModeKHR mode)
{
    switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "VK_PRESENT_MODE_IMMEDIATE_KHR";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "VK_PRESENT_MODE_MAILBOX_KHR";
    case VK_PRESENT_MODE_FIFO_KHR: return "VK_PRESENT_MODE_FIFO_KHR";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "VK_PRESENT_MODE_FIFO_RELAXED_KHR";
    default: return "a present mode this port does not name";
    }
}

// Which of the surface's formats to use.
//
// A request, not a demand. The surface owns the list -- on X11 it is whatever the window's visual can do,
// and a format the window cannot show is not a format this layer gets to choose. B8G8R8A8_UNORM is what
// the game draws with (A8R8G8B8 on the console) and it is deliberately *not* the _SRGB variant of it: the
// game writes gamma-corrected values itself, and an sRGB view would encode them a second time.
VkSurfaceFormatKHR choose_surface_format(const std::vector<VkSurfaceFormatKHR> &formats, VkFormat preferred)
{
    // The colour space is part of the format: several drivers list the same VkFormat once per colour space,
    // and the surface's currentTransform-free "capabilities" struct is not a substitute for picking one.
    for (const VkSurfaceFormatKHR &f : formats)
        if (f.format == preferred && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return f;
    for (const VkSurfaceFormatKHR &f : formats)
        if (f.format == preferred) return f;
    for (const VkSurfaceFormatKHR &f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) return f;
    return formats.empty() ? VkSurfaceFormatKHR() : formats[0];
}

// Free the swapchain's own objects without touching the surface. Split out of swapchain_destroy because a
// rebuild destroys exactly these and nothing else.
void swapchain_release()
{
    if (g_device && g_swapchain) vkDeviceWaitIdle(g_device);
    for (size_t i = 0; i < g_swapchain_images.size(); i++)
        if (g_device && g_swapchain_images[i].view)
            vkDestroyImageView(g_device, g_swapchain_images[i].view, nullptr);
    if (g_device && g_swapchain && pfnDestroySwapchain) pfnDestroySwapchain(g_device, g_swapchain, nullptr);
    g_swapchain = VK_NULL_HANDLE;
    g_swapchain_images.clear();
    g_swapchain_handles.clear();
    g_swapchain_width = 0;
    g_swapchain_height = 0;
    g_swapchain_format = VK_FORMAT_UNDEFINED;
    g_swapchain_present_mode = "none";
}

} // namespace

void swapchain_invalidate()
{
    // Mark the swapchain as stale so the next acquire will rebuild it.
    // This is called when the window has been resized or the driver returned SUBOPTIMAL/OUT_OF_DATE.
    g_swapchain_stale = true;
}

void vulkan_want_instance_extension(const char *name)
{
    if (!name || !name[0]) return;
    for (const std::string &existing : g_want_instance_extensions) {
        if (existing == name) return; // already present
    }
    g_want_instance_extensions.push_back(name);
    logf("[gfx] added instance extension: %s", name);
}

void swapchain_destroy()
{
    swapchain_release();
    g_swapchain_stale = false;
}

bool swapchain_create(VkFormat preferred_format, bool immediate)
{
    if (!ready()) {
        g_error = "gfx::init has not run, so there is no device to make a swapchain with";
        return false;
    }
    if (!g_surface || !pfnSurfaceCapabilities) {
        g_error = "there is no Vulkan surface: the window path has to register one before gfx::init";
        return false;
    }
    if (g_present_family == UINT32_MAX) {
        g_error = "this device cannot present to the window: no queue family accepts the surface";
        return false;
    }

    VkSurfaceCapabilitiesKHR caps{};
    if (pfnSurfaceCapabilities(g_physical, g_surface, &caps) != VK_SUCCESS) {
        g_error = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed";
        return false;
    }

    // A zero extent is a window with no drawable area: minimised, or on another workspace. It is not an
    // error and the answer is not to build a swapchain of zero by zero -- Vulkan treats a zero extent as
    // "undefined size", and every image in it is useless. The existing swapchain stays, and the caller
    // tries again when there is somewhere to draw.
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) {
        g_error = "the window has no drawable area (minimised); the swapchain was left as it was";
        return false;
    }

    uint32_t format_count = 0;
    pfnSurfaceFormats(g_physical, g_surface, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    if (format_count) pfnSurfaceFormats(g_physical, g_surface, &format_count, formats.data());
    if (formats.empty()) {
        g_error = "the surface reports no formats at all";
        return false;
    }

    // TRANSFER_SRC as well as TRANSFER_DST: the drawable is copied out of as well as into. Nothing in the
    // present path needs that today, but a swapchain created without it cannot be read back at all, and
    // reading a drawable back is how a test can prove the frame it presented was really the frame it drew
    // (see tests/test_window.cpp).
    const VkImageUsageFlags want_usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const VkSurfaceFormatKHR format =
        choose_surface_format(formats, preferred_format ? preferred_format : VK_FORMAT_B8G8R8A8_UNORM);

    // The usage a swapchain may ask for is what the surface supports. Per-format usage is only in
    // VkSurfaceFormat2KHR, which is VK_KHR_surface_format_2; the core VkSurfaceFormatKHR carries a format and
    // a colour space and nothing else, so a driver that wants to restrict a format's usage says so through
    // an extension this layer does not ask for. Asking for a bit the surface does not support is invalid
    // usage, and with no validation layers installed the driver answers that however it likes.
    VkImageUsageFlags usage = want_usage & caps.supportedUsageFlags;
    if (!(usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        g_error = "this surface cannot be a colour attachment at all, so nothing could be drawn into a "
                  "swapchain built on it";
        return false;
    }
    g_swapchain_can_read_back = (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (!g_swapchain_can_read_back)
        logf("[gfx] this surface cannot copy out of its drawables, so the window test skips the readback\n");
    usage &= ~VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    // minImageCount + 1 so that the presentation engine always has one image to work with while another is
    // being drawn -- the classic reason for asking for more than the minimum.
    // **maxImageCount of zero means "no limit", not "none."** Clamping to it asks for a zero-image
    // swapchain, and this is what the X11 surface on this machine reports (measured: min 3, max 0).
    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount != 0 && image_count > caps.maxImageCount) image_count = caps.maxImageCount;

    uint32_t mode_count = 0;
    pfnSurfacePresentModes(g_physical, g_surface, &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    if (mode_count) pfnSurfacePresentModes(g_physical, g_surface, &mode_count, modes.data());
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR; // the only one every implementation must have
    if (immediate)
        for (VkPresentModeKHR m : modes)
            if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) present_mode = m;

    const VkSurfaceFormatKHR format_unused_guard = format;
    (void)format_unused_guard;

    VkSwapchainCreateInfoKHR sci;
    memset(&sci, 0, sizeof sci);
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = g_surface;
    sci.minImageCount = image_count;
    sci.imageFormat = format.format;
    sci.imageColorSpace = format.colorSpace;
    sci.imageExtent = caps.currentExtent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = usage;
    // Exclusive even across two families: the semaphore pair below moves ownership, and that is what the
    // acquire semaphore is for. Concurrent sharing with a second queue would be a second way to be wrong.
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    // OPAQUE when the surface offers it, which is the one that costs the compositor nothing and is what a
    // window with no alpha wants. supportedCompositeAlpha is a mask of bits, not a list: one of these three
    // is always supported, but not always the one asked for first.
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
        sci.compositeAlpha = caps.supportedCompositeAlpha
                                 ? (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & (~caps.supportedCompositeAlpha + 1u))
                                 : VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = present_mode;
    sci.clipped = VK_TRUE;
    // Handing the old swapchain over lets the driver reuse its images instead of allocating new ones, which
    // is what keeps a resize from stalling. The old views are destroyed only after the new ones exist,
    // because on a driver that reuses the images the new views are made from them.
    sci.oldSwapchain = g_swapchain;

    VkSwapchainKHR created = VK_NULL_HANDLE;
    const VkResult r = pfnCreateSwapchain(g_device, &sci, nullptr, &created);
    if (r != VK_SUCCESS || created == VK_NULL_HANDLE) {
        g_error = std::string("vkCreateSwapchainKHR: ") + vk_result_name(r);
        return false;
    }

    uint32_t count = 0;
    pfnSwapchainImages(g_device, created, &count, nullptr);
    std::vector<VkImage> handles(count);
    if (count) pfnSwapchainImages(g_device, created, &count, handles.data());
    std::vector<Image> images(count);

    for (uint32_t i = 0; i < count; i++) {
        VkImageViewCreateInfo vci;
        memset(&vci, 0, sizeof vci);
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = handles[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(g_device, &vci, nullptr, &images[i].view) != VK_SUCCESS) {
            for (uint32_t j = 0; j < i; j++) vkDestroyImageView(g_device, images[j].view, nullptr);
            pfnDestroySwapchain(g_device, created, nullptr);
            g_error = "vkCreateImageView on a swapchain image failed";
            return false;
        }
        images[i].image = handles[i];
        images[i].memory = VK_NULL_HANDLE; // the swapchain owns it; this layer owns only the view
        images[i].width = caps.currentExtent.width;
        images[i].height = caps.currentExtent.height;
        images[i].format = format.format;
        images[i].mip_levels = 1;
        images[i].layout_slot = new_layout_slot();
        layout_of(images[i]) = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    swapchain_release(); // the old one, which may have lent its images to the new one
    g_swapchain = created;
    g_swapchain_images = images;
    g_swapchain_handles = handles;
    g_swapchain_width = caps.currentExtent.width;
    g_swapchain_height = caps.currentExtent.height;
    g_swapchain_format = format.format;
    g_swapchain_present_mode = present_mode_name(present_mode);
    g_swapchain_stale = false;
    g_swapchain_wants_immediate = immediate;
    g_swapchain_generation++;

    if (!g_acquire_done) {
        VkSemaphoreCreateInfo sci_sem;
        memset(&sci_sem, 0, sizeof sci_sem);
        sci_sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(g_device, &sci_sem, nullptr, &g_acquire_done) != VK_SUCCESS ||
            vkCreateSemaphore(g_device, &sci_sem, nullptr, &g_release_done) != VK_SUCCESS) {
            g_error = "could not create the two semaphores a frame's hand-over needs";
            swapchain_release();
            return false;
        }
    }

    logf("[gfx] swapchain %ux%u, %u images, format %d, %s (generation %u)\n", g_swapchain_width,
         g_swapchain_height, image_count, (int)format.format, g_swapchain_present_mode, g_swapchain_generation);
    return true;
}

bool swapchain_recreate() { return swapchain_create(g_swapchain_format, g_swapchain_wants_immediate); }

bool swapchain_acquire(uint32_t *index)
{
    if (!index) return false;
    if (!g_swapchain) {
        g_error = "there is no swapchain to acquire from: call swapchain_create first";
        return false;
    }
    // A stale swapchain is rebuilt once, here, rather than being handed to the caller as a failed frame.
    // Both ends of the window's life -- a resize the driver noticed and a resize it did not (measured:
    // on X11/llvmpipe a stale swapchain keeps returning VK_SUCCESS long after the window changed, so the
    // return code alone cannot drive this) -- arrive at the same place, which is one rebuild here.
    if (g_swapchain_stale && !swapchain_recreate()) return false;

    // UINT64_MAX: wait as long as it takes. A finite timeout would buy a frame that has nothing to draw
    // into, and the loader has no second thing to do while the window is not ready.
    const VkResult r = pfnAcquireNextImage(g_device, g_swapchain, UINT64_MAX, g_acquire_done, VK_NULL_HANDLE,
                                              index);
    if (r == VK_SUCCESS) return true;
    // SUBOPTIMAL is not a failure: the image is valid and the frame is worth showing. It says the surface
    // no longer matches the swapchain, which is a rebuild request for later rather than an error now.
    if (r == VK_SUBOPTIMAL_KHR) {
        g_swapchain_stale = true;
        logf("[gfx] acquire returned SUBOPTIMAL: the swapchain will be rebuilt\n");
        return true;
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) {
        g_swapchain_stale = true;
        g_error = std::string("the window went away from under the swapchain (") + vk_result_name(r) + ")";
        if (r == VK_ERROR_SURFACE_LOST_KHR) return false;
        return swapchain_recreate() && pfnAcquireNextImage(g_device, g_swapchain, UINT64_MAX, g_acquire_done,
                                                             VK_NULL_HANDLE, index) == VK_SUCCESS;
    }
    g_error = std::string("vkAcquireNextImageKHR: ") + vk_result_name(r);
    return false;
}

bool swapchain_present(uint32_t index)
{
    if (!g_swapchain) {
        g_error = "there is no swapchain to present to";
        return false;
    }
    if (index >= g_swapchain_images.size()) {
        g_error = "the drawable index is not one of this swapchain's images";
        return false;
    }

    // The frame is submitted here rather than by the caller, because the submission is what hands the
    // image over: it waits on the acquire semaphore and signals the release one, and a caller that ended
    // the frame itself would end it without either and present an image the GPU had not finished with.
    if (!submit_frame(g_acquire_done, g_release_done, &g_swapchain_images[index])) return false;

    VkPresentInfoKHR pi;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g_release_done;
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_swapchain;
    pi.pImageIndices = &index;
    const VkResult r = pfnQueuePresent(g_present_queue, &pi);

    // Both queues, not just the graphics one: when presentation lives on another family the release has
    // happened on a queue this thread has not touched, and reusing the acquire semaphore before the
    // presentation engine is done with it is the use-after-free that this wait is here to prevent.
    vkQueueWaitIdle(g_queue);
    if (g_present_queue != g_queue) vkQueueWaitIdle(g_present_queue);

    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
        if (r == VK_SUBOPTIMAL_KHR) g_swapchain_stale = true;
        return true;
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) {
        g_swapchain_stale = true;
        g_error = std::string("the presentation engine refused this frame: ") + vk_result_name(r);
        return r == VK_ERROR_OUT_OF_DATE_KHR;
    }
    g_error = std::string("vkQueuePresentKHR: ") + vk_result_name(r);
    return false;
}

// ---- frames ----

namespace {
VkViewport g_viewport{0, 0, 1, 1, 0, 1};
VkRect2D g_scissor{{0, 0}, {1, 1}};
bool g_scissor_set = false;
const Buffer *g_bound_buffer;// The codes are xd3d::VertexFormatCode, spelled as numbers here on purpose: including d3d/vertex_posix.h
// would work, but the rule that d3d/ must not see Vulkan's types is much easier to keep true when the two
// sides share a numbering instead of a header. The static_asserts below are the guard -- they compare this
// table's codes against the header's enum, so the two cannot drift apart silently.
} // namespace

void set_render_target(Image *color, Image *depth)
{
    // Ending the current pass first is what makes SetRenderTarget legal mid-frame: the attachments a render
    // pass began with must not change while it is open, and Vulkan (like the Xbox's push buffer) has
    // nothing to say about the alternative.
    if (g_recording && (color != g_color || depth != g_depth)) end_pass();
    g_color = color;
    g_depth = depth;
}

bool begin_frame()
{
    if (!ready() || g_recording) return g_recording;
    if (!g_color || !g_color->view) {
        g_error = "no render target: call set_render_target first";
        return false;
    }

    vkResetCommandBuffer(g_cmd, 0);
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(g_cmd, &bi) != VK_SUCCESS) {
        g_error = "vkBeginCommandBuffer failed";
        return false;
    }
    g_recording = true;

    // A target that has been sampled (a texture the game locked) or copied out (a readback) is not in the
    // layout a pass needs. The move goes into this same command buffer, before the pass opens -- which is
    // legal, and means a frame that needs a transition is still one submission. An earlier version recorded
    // these into a command buffer that had not been begun, which the Intel driver turned into a crash: a
    // vkCmd* call with no recording is undefined behaviour, not a no-op.
    if (layout_of(*g_color) != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        barrier(g_color->image, layout_of(*g_color), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        layout_of(*g_color) = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (g_depth && g_depth->view && layout_of(*g_depth) != VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) {
        barrier(g_depth->image, layout_of(*g_depth), VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
        layout_of(*g_depth) = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    }

    g_scissor_set = false; // a frame starts with no scissor, which is a different thing from a 1x1 one
    g_pipeline_bound = false; // and with no pipeline, which a new frame has to be told about again
    // Deliberately no pass here. A pass is opened by the first thing that draws -- see ensure_pass -- because
    // a buffer copy or a layout barrier recorded between here and there is legal, and recorded after it is
    // not. Opening the pass eagerly is what put every upload inside one.
    return true;
}

bool pipeline_is_bound() { return g_pipeline_bound; }

bool set_pipeline(const Pipeline *p)
{
    if (!ensure_pass() || !p || !p->pipeline) return false;
    vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
    g_pipeline_bound = true;
    return true;
}

bool set_viewport(float x, float y, float w, float h, float min_z, float max_z)
{
    if (!ensure_pass()) return false;
    // No Y flip, which is the thing worth being sure about: D3D's NDC y = -1 is the *top* of the viewport and
    // Vulkan's is too, so a positive height maps the console's picture straight through. An earlier version
    // negated the height "to flip Y like the Xbox" and drew the game upside down -- the console convention is
    // already the one Vulkan uses.
    g_viewport.x = x;
    g_viewport.y = y;
    g_viewport.width = w;
    g_viewport.height = h;
    g_viewport.minDepth = min_z;
    g_viewport.maxDepth = max_z;
    vkCmdSetViewport(g_cmd, 0, 1, &g_viewport);
    return true;
}

bool set_scissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!ensure_pass()) return false;
    g_scissor_set = true;
    // The Xbox's scissor rectangle is unsigned, and Vulkan's offsets are signed while its extent is not,
    // so the conversions are spelled out. A caller that computed a negative width would otherwise get a
    // rectangle Vulkan quietly ignores.
    g_scissor.offset.x = (int32_t)x;
    g_scissor.offset.y = (int32_t)y;
    g_scissor.extent.width = w;
    g_scissor.extent.height = h;
    vkCmdSetScissor(g_cmd, 0, 1, &g_scissor);
    return true;
}

bool bind_vertex_buffer(const Buffer *b, uint32_t binding, VkDeviceSize offset)
{
    if (!ensure_recording() || !b || !b->buffer) return false;
    vkCmdBindVertexBuffers(g_cmd, binding, 1, &b->buffer, &offset);
    g_bound_buffer = b;
    g_bound_binding = binding;
    return true;
}

bool bind_index_buffer(const Buffer *b, VkIndexType type)
{
    if (!ensure_recording() || !b || !b->buffer) return false;
    vkCmdBindIndexBuffer(g_cmd, b->buffer, 0, type);
    g_bound_buffer = b;
    return true;
}

bool bind_index_buffer_upload(const void *indices, VkDeviceSize bytes)
{
    if (!indices || bytes == 0) return false;
    if (bytes > g_index_capacity) {
        // Rounded up so that a mesh that grows by a handful of indices does not reallocate. Power of two,
        // because that is the only growth policy that never reallocates more than half the time.
        uint64_t want = g_index_capacity ? g_index_capacity : 4096;
        while (want < (uint64_t)bytes) want *= 2;
        if (g_index_buffer.buffer) destroy_buffer(&g_index_buffer);
        if (!create_buffer(&g_index_buffer, want, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true)) return false;
        g_index_capacity = want;
    }
    if (!update_buffer(&g_index_buffer, 0, bytes, indices)) return false;
    return bind_index_buffer(&g_index_buffer, VK_INDEX_TYPE_UINT16);
}

bool draw(Topology topology, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex)
{
    if (!ensure_pass()) return false;
    vkCmdDraw(g_cmd, vertex_count, instance_count, first_vertex, (VkPrimitiveTopology)topology);
    return true;
}

bool draw_indexed(Topology topology, uint32_t index_count, uint32_t instance_count, uint32_t first_index,
                  int32_t vertex_offset)
{
    if (!ensure_pass()) return false;
    vkCmdDrawIndexed(g_cmd, index_count, instance_count, first_index, vertex_offset,
                     (VkPrimitiveTopology)topology);
    return true;
}

bool set_scissor_enable(bool on)
{
    (void)on;
    return true;
}

namespace {

// End the pass, submit, wait, and hand back the staging the frame borrowed. This is the tail of end_frame
// and it is also the tail of the present path, because those are the same three steps with two extra
// semaphones in the middle of them -- and one function means the acquire/release pair cannot be forgotten
// in one of the two callers or used in the wrong order in the other.
//
// `wait` is the semaphore that says the presentation engine has handed the drawable over and `signal` the
// one that says the GPU is finished with it; both are null for a frame that is not going to a screen.
// `present_target` is the drawable, and it gets its layout transition recorded here because that
// transition is the last thing the command buffer has to do before vkQueuePresentKHR is allowed to mean
// anything.
bool submit_frame(VkSemaphore wait, VkSemaphore signal, Image *present_target)
{
    if (!ready()) {
        g_error = "gfx::init has not run";
        return false;
    }

    if (!g_recording) {
        // A frame that never drew anything still owns a drawable, and a drawable has to be in
        // PRESENT_SRC_KHR to be presented. Recorded on its own rather than refused: "the game drew nothing
        // this frame" is a normal thing for a game to do, and the alternative is a caller that has to know
        // this and branch on it.
        if (!present_target) return true;
        vkResetCommandBuffer(g_cmd, 0);
        VkCommandBufferBeginInfo bi;
        memset(&bi, 0, sizeof bi);
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(g_cmd, &bi) != VK_SUCCESS) {
            g_error = "vkBeginCommandBuffer failed";
            return false;
        }
    }

    end_pass();
    g_recording = false;

    if (present_target) {
        const VkImageLayout from = layout_of(*present_target);
        if (from != VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
            barrier(present_target->image, from, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, 0,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        layout_of(*present_target) = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }

    // Ended before it is submitted. Submitting a command buffer that is still recording is invalid usage,
    // and llvmpipe 1.2 tolerates it for a pass and a draw but segfaults once the stream also holds a
    // vkCmdCopyBuffer -- which is what the managed buffer's upload is, and what made this crash only once a
    // test actually flushed a buffer. Nothing had been caught here before because nothing else in the frame
    // tripped it.
    if (vkEndCommandBuffer(g_cmd) != VK_SUCCESS) return fail("vkEndCommandBuffer", VK_ERROR_UNKNOWN);

    VkSubmitInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cmd;
    if (wait != VK_NULL_HANDLE) {
        // COLOR_ATTACHMENT_OUTPUT on the waiting side, because that is the first stage that touches the
        // drawable: anything earlier would let the frame start drawing into an image the presentation
        // engine still owns, which is the one race a swapchain has.
        const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        si.pWaitDstStageMask = &stage;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &wait;
    }
    if (signal != VK_NULL_HANDLE) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &signal;
    }
    VkResult r = vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) return fail("vkQueueSubmit", r);
    // Waited on rather than signalled: a frame that is not finished when the loader looks at it is worse
    // than a slow frame, and the D3D8 layer's present path decides when to wait anyway.
    r = vkQueueWaitIdle(g_queue);
    if (r != VK_SUCCESS) return fail("vkQueueWaitIdle", r);

    // The frame has finished, so every copy it named has run and the staging it borrowed can go. Leaving
    // them would be merely wasteful; releasing them early is what crashes.
    for (size_t i = 0; i < g_staging_pending.size(); i++) destroy_buffer(&g_staging_pending[i]);
    g_staging_pending.clear();
    return true;
}

} // namespace

bool end_frame() { return submit_frame(VK_NULL_HANDLE, VK_NULL_HANDLE, nullptr); }

// The rectangle a clear covers: the scissor when one has been set, otherwise the whole target. Vulkan
// requires a rectangle either way, and the value this layer starts with is 1x1 -- which turned every Clear
// into a one-pixel clear until this said so. The Xbox's Clear with a null rectangle clears the whole target
// too, which is why the default here is the whole target rather than the scissor.
static VkClearRect clear_rect()
{
    VkClearRect rect;
    memset(&rect, 0, sizeof rect);
    rect.baseArrayLayer = 0;
    rect.layerCount = 1;
    if (g_scissor_set) {
        rect.rect = g_scissor;
    } else if (g_color) {
        rect.rect = {{0, 0}, {g_color->width, g_color->height}}; // VkExtent2D is unsigned, offsets are not
    }
    return rect;
}

bool clear_color(const float rgba[4])
{
    memcpy(g_pass_clear_colour, rgba, sizeof g_pass_clear_colour);
    g_pass_clear = true;

    // No frame yet: the clear becomes the next pass's loadOp, which begin_pass consumes. The pass has to be
    // opened here rather than left for the next draw, because a loadOp is fixed when the pass begins -- and a
    // pass opened by a draw afterwards would load whatever was there and throw the clear away. That is what
    // beginning the frame eagerly used to do for free, and losing it silently drops the clear.
    if (!g_recording) {
        if (!begin_frame()) return false;
        return begin_pass(g_color, g_depth) != VK_NULL_HANDLE;
    }

    // A frame in progress: close the pass and open another on the same attachments that starts cleared. A pass
    // may end and another begin on the same attachments, which makes this a legal mid-frame clear -- and on
    // this driver it is the one that works.
    end_pass();
    return begin_pass(g_color, g_depth) != VK_NULL_HANDLE;
}

// Record a vkCmdClearAttachments that is alone in its pass.
//
// This is not tidiness, it is the measurement in the comment on clear_color: on 32-bit llvmpipe a colour
// vkCmdClearAttachments inside a dynamic-rendering pass stops every *later* draw in that pass from producing
// a fragment. A whole-target clear avoids it by being the pass's loadOp instead. A rectangle has no loadOp,
// so the only way to give it one is to close the pass, open a fresh one, clear, and close that one again --
// leaving the clear with nothing after it that it can spoil.
//
// The cost is a pass boundary per rectangle, which is why a clear covering the whole target takes the
// loadOp path instead and never comes here.
bool clear_attachment_alone(const VkClearAttachment &ca, const VkClearRect &rect)
{
    if (!ensure_recording()) return false;
    end_pass(); // whatever the game was drawing into ends here
    if (!begin_pass(g_color, g_depth)) return false;
    vkCmdClearAttachments(g_cmd, 1, &ca, 1, &rect);
    end_pass(); // and the clear ends here, before the next draw
    return true;
}

bool clear_color_rect(const float rgba[4], uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    // A rectangle that covers everything is the whole-target clear wearing a hat, and the loadOp path is
    // both cheaper and the one this driver is known to handle. The game's own Clear passes one full-screen
    // rectangle constantly.
    if (g_color && x == 0 && y == 0 && w >= g_color->width && h >= g_color->height) return clear_color(rgba);

    VkClearAttachment ca;
    memset(&ca, 0, sizeof ca);
    ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ca.colorAttachment = 0;
    ca.clearValue.color = {{rgba[0], rgba[1], rgba[2], rgba[3]}};
    VkClearRect rect;
    memset(&rect, 0, sizeof rect);
    rect.rect = {{(int32_t)x, (int32_t)y}, {w, h}};
    rect.baseArrayLayer = 0;
    rect.layerCount = 1;
    return clear_attachment_alone(ca, rect);
}

// Depth is cleared with vkCmdClearAttachments rather than with the pass's loadOp, because this is the form
// that survives a rectangle and because a depth clear does not stop later draws on the driver in question.
bool clear_depth(float value)
{
    // Alone in its pass, for the same reason the rectangle clear is: see clear_attachment_alone. The
    // measurement that forced this was made for colour; depth is treated the same way because it costs the
    // same and the alternative is a draw that silently produces nothing.
    VkClearAttachment ca;
    memset(&ca, 0, sizeof ca);
    ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    ca.clearValue.depthStencil = {value, 0};
    const VkClearRect rect = clear_rect();
    return clear_attachment_alone(ca, rect);
}

bool read_color(const Image *img, void *out)
{
    if (!img || !img->image || !out) return false;

    // Whatever the caller had open is closed first: attachments a pass began with cannot change under it,
    // and the copy has to run with the image in TRANSFER_SRC.
    if (g_recording) {
        end_pass();
        g_recording = false;
    }

    const VkDeviceSize bytes = (VkDeviceSize)img->width * img->height * 4;
    VkBuffer dst;
    VkDeviceMemory dst_mem = VK_NULL_HANDLE;
    void *mapped = nullptr;
    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof bci);
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(g_device, &bci, nullptr, &dst) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(g_device, dst, &req);
    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof mai);
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex =
        find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(g_device, &mai, nullptr, &dst_mem) != VK_SUCCESS ||
        vkMapMemory(g_device, dst_mem, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        vkDestroyBuffer(g_device, dst, nullptr);
        if (dst_mem) vkFreeMemory(g_device, dst_mem, nullptr);
        return false;
    }
    vkBindBufferMemory(g_device, dst, dst_mem, 0);

    vkResetCommandBuffer(g_cmd, 0);
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g_cmd, &bi);

    barrier(img->image, layout_of(*img), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {img->width, img->height, 1};
    vkCmdCopyImageToBuffer(g_cmd, img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &copy);

    // Back to the layout the image had, which is almost always COLOR_ATTACHMENT_OPTIMAL: the next frame's
    // pass expects it there and begin_frame() would otherwise move it a second time.
    barrier(img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout_of(*img), VK_ACCESS_TRANSFER_READ_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    VkSubmitInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cmd;
    vkEndCommandBuffer(g_cmd); // as in end_frame: a buffer is submitted in the executable state
    vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_queue);

    memcpy(out, mapped, (size_t)bytes);
    vkUnmapMemory(g_device, dst_mem);
    vkDestroyBuffer(g_device, dst, nullptr);
    vkFreeMemory(g_device, dst_mem, nullptr);
    return true;
}

bool write_png_from_color_target(const char *path, const Image *img)
{
    if (!path || !img || !img->image || img->width == 0 || img->height == 0) return false;
    std::vector<uint8_t> pixels((size_t)img->width * img->height * 4);
    if (!read_color(img, pixels.data())) return false;

    // read_color hands back the image's own byte order; PNG wants RGBA. Only the two formats this port
    // creates for colour targets differ, and only in the first and third byte.
    const bool bgra = img->format == VK_FORMAT_B8G8R8A8_UNORM || img->format == VK_FORMAT_B8G8R8A8_SRGB;
    if (bgra) {
        for (size_t i = 0; i < pixels.size(); i += 4) {
            const uint8_t t = pixels[i];
            pixels[i] = pixels[i + 2];
            pixels[i + 2] = t;
        }
    }
    return write_png(path, pixels.data(), img->width, img->height);
}


} // namespace gfx
