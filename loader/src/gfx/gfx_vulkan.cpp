#include "gfx/gfx_vulkan.h"

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
    return g_cmd;
}

void end_pass()
{
    if (!g_recording) return;
    vkCmdEndRendering(g_cmd);
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
    // It used to be a transfer: this buffer plus `vkCmdFillBuffer`. That is where the 32-bit Intel driver
    // segfaults, measured (loader/tools/vk_intel_diag.cpp case G: empty submits, dynamic rendering, memory,
    // images and descriptor allocation all survive; the first transfer command does not). It is a poor probe,
    // not because the driver is unusable but because **the game never issues a transfer command** -- it draws
    // and it writes push buffers -- so the probe was judging the driver by work this port will never ask of it,
    // and turning a usable GPU into "crashes on submit". A barrier is a real command in the same path, and it
    // is one the game's own workload does contain.
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
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            type = i;
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

VkCommandBuffer ensure_recording()
{
    if (!g_recording && !begin_frame()) return VK_NULL_HANDLE;
    return g_cmd;
}

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
    VkResult r = vkCreateInstance(&ici, nullptr, &g_instance);
    if (r != VK_SUCCESS) return fail("no Vulkan instance", r);

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
    g_adapter.present_supported = true;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof qci);
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = g_queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

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
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &features;
    dci.pNext = &features13;
    r = vkCreateDevice(g_physical, &dci, nullptr, &g_device);
    if (r != VK_SUCCESS) return fail("no Vulkan device", r);

    if (!have_dynamic_rendering) {
        // Without this feature there is no vkCmdBeginRendering, so the render path cannot work at all, and
        // saying so here is much better than letting the first frame die inside a driver.
        return fail("this driver has no dynamic rendering (Vulkan 1.2 feature), and this layer draws with it",
                    r);
    }

    vkGetDeviceQueue(g_device, g_queue_family, 0, &g_queue);

    VkCommandPoolCreateInfo pci;
    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = g_queue_family;
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
    if (g_device && g_pool) vkDestroyCommandPool(g_device, g_pool, nullptr);
    if (g_device) vkDestroyDevice(g_device, nullptr);
    if (g_instance) vkDestroyInstance(g_instance, nullptr);
    g_device = VK_NULL_HANDLE;
    g_instance = VK_NULL_HANDLE;
    g_physical = VK_NULL_HANDLE;
    g_queue = VK_NULL_HANDLE;
    g_pool = VK_NULL_HANDLE;
    g_cmd = VK_NULL_HANDLE;
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
            b->buffer = b->memory = VK_NULL_HANDLE;
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
    destroy_buffer(&staging);
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
        img->image = img->memory = img->view = VK_NULL_HANDLE;
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
        v.format = desc.attributes[i].format;
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

// ---- frames ----

namespace {
Image *g_color;
Image *g_depth;
VkViewport g_viewport{0, 0, 1, 1, 0, 1};
VkRect2D g_scissor{{0, 0}, {1, 1}};
bool g_scissor_set = false;
const Buffer *g_bound_buffer;
uint32_t g_bound_binding = 0;
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
    if (!begin_pass(g_color, g_depth)) {
        g_recording = false;
        return false;
    }
    return true;
}

bool set_pipeline(const Pipeline *p)
{
    if (!ensure_recording() || !p || !p->pipeline) return false;
    vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
    return true;
}

bool set_viewport(float x, float y, float w, float h, float min_z, float max_z)
{
    if (!ensure_recording()) return false;
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
    if (!ensure_recording()) return false;
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

bool draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex)
{
    if (!ensure_recording()) return false;
    vkCmdDraw(g_cmd, vertex_count, instance_count, first_vertex, 0);
    return true;
}

bool draw_indexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset)
{
    if (!ensure_recording()) return false;
    vkCmdDrawIndexed(g_cmd, index_count, instance_count, first_index, vertex_offset, 0);
    return true;
}

bool set_scissor_enable(bool on)
{
    (void)on;
    return true;
}

bool end_frame()
{
    if (!g_recording) return true;
    end_pass();
    g_recording = false;

    VkSubmitInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cmd;
    VkResult r = vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) return fail("vkQueueSubmit", r);
    // Waited on rather than signalled: a frame that is not finished when the loader looks at it is worse
    // than a slow frame, and the D3D8 layer's present path decides when to wait anyway.
    r = vkQueueWaitIdle(g_queue);
    if (r != VK_SUCCESS) return fail("vkQueueWaitIdle", r);
    return true;
}

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

    // No frame yet: the clear becomes the next pass's loadOp, which begin_pass consumes.
    if (!g_recording) return begin_frame();

    // A frame in progress: close the pass and open another on the same attachments that starts cleared. A pass
    // may end and another begin on the same attachments, which makes this a legal mid-frame clear -- and on
    // this driver it is the one that works.
    end_pass();
    return begin_pass(g_color, g_depth) != VK_NULL_HANDLE;
}

bool clear_color_rect(const float rgba[4], uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!g_recording && !begin_frame()) return false;
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
    vkCmdClearAttachments(g_cmd, 1, &ca, 1, &rect);
    return true;
}

// Depth is cleared with vkCmdClearAttachments rather than with the pass's loadOp, because this is the form
// that survives a rectangle and because a depth clear does not stop later draws on the driver in question.
bool clear_depth(float value)
{
    if (!g_recording && !begin_frame()) return false;
    VkClearAttachment ca;
    memset(&ca, 0, sizeof ca);
    ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    ca.clearValue.depthStencil = {value, 0};
    const VkClearRect rect = clear_rect();
    vkCmdClearAttachments(g_cmd, 1, &ca, 1, &rect);
    return true;
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
