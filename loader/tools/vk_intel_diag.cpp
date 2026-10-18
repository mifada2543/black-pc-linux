// Why does the 32-bit Intel driver die on submit, and is it the driver or us?
//
// A0 found that `Intel(R) Graphics (ADL GT2)` segfaults inside `vkQueueSubmit` from a 32-bit process, while
// llvmpipe is fine. "The driver is broken" is one of three possible answers, and they lead to different
// architectures, so this program asks the driver three questions, each in its own forked child so that the
// crash is an answer rather than an exit:
//
//   A. Vulkan 1.0 instance, an *empty* command buffer submitted.
//   B. Vulkan 1.3 instance, the same empty submit.
//   C. Vulkan 1.3 instance, a submit that begins dynamic rendering.
//
// Reading the results:
//   * A crashes  -> the driver cannot take a submit at all in 32 bits. Nothing in this port can fix it, and
//                   the only route to Intel is A0b (a 64-bit process draws).
//   * A works, B crashes -> the fault is in what 1.3 asks of it. This port asks for 1.3 because dynamic
//                   rendering is 1.3, so the fallback is render-pass objects (and `VK_KHR_dynamic_rendering`
//                   dropped). Work, but it is work inside this process.
//   * A and B work, C crashes -> the fault is dynamic rendering specifically, and only the passes that
//                   render need the older path.
//
// This is a diagnostic, not a test: it prints and exits 0 whatever happens, because the point is to record
// the answer, not to fail a suite. The finding is written up in `NOTES.md` ("A0 -- gate Vulkan 32-bit").
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <stdint.h>

#include <vector>

#include <vulkan/vulkan.h>

namespace {

// What happens before the submit. A, B and C are empty of everything; D onwards build up one piece at a
// time of what gfx::init() does before it submits, because "the crash is in vkQueueSubmit" means *this*
// call's earlier work left something the driver then trips over, and the only way to find which is to add
// one piece at a time and see which one makes it die.
enum Work {
    WORK_EMPTY,       // nothing at all
    WORK_DYNAMIC,     // a dynamic rendering begin/end in the command buffer
    WORK_MEMORY,      // allocate and bind device memory for a buffer
    WORK_IMAGE,       // a small image
    WORK_DESCRIPTORS, // a descriptor set layout, a pool, and an allocated set (nothing written into it)
    WORK_FILL,        // vkCmdFillBuffer on a created, memory-bound transfer destination
    WORK_BARRIER,      // a memory barrier at the top-of-pipe/bottom-of-pipe stages
    WORK_BARRIER_HOST, // the same barrier with HOST as the source stage: what the loader's probe records
    WORK_AS_LOADER,     // the loader's probe, step for step: HOST_VISIBLE memory without COHERENT, and a barrier
    WORK_FEATURES,      // the same, but with a zeroed VkPhysicalDeviceFeatures passed as pEnabledFeatures
    WORK_SECOND_INST,   // a second instance in the child, because the parent already has one (init forks)
    WORK_IMAGE_BARRIER, // image + its memory + a layout-transition barrier, usage without TRANSFER_DST
    WORK_IMAGE_XFER,    // the same, but with TRANSFER_DST in the usage flags
    WORK_IMAGE_LOCAL,   // image + DEVICE_LOCAL memory + barrier: exactly what create_color_target() does
    WORK_DEPTH,         // the same, but a D32_SFLOAT depth image: the other half of create_targets
    WORK_REENUM,        // as WORK_AS_LOADER, but the child enumerates its own VkPhysicalDevice (probe_child does)
};

struct Case {
    const char *name;
    uint32_t api_version;
    Work work;
};

const Case kCases[] = {
    {"A: api 1.0, empty submit", VK_API_VERSION_1_0, WORK_EMPTY},
    {"B: api 1.3, empty submit", VK_API_VERSION_1_3, WORK_EMPTY},
    {"C: api 1.3, dynamic rendering", VK_API_VERSION_1_3, WORK_DYNAMIC},
    {"D: + device memory for a buffer", VK_API_VERSION_1_3, WORK_MEMORY},
    {"E: + an image", VK_API_VERSION_1_3, WORK_IMAGE},
    {"F: + descriptor set allocate", VK_API_VERSION_1_3, WORK_DESCRIPTORS},
    {"G: + vkCmdFillBuffer (transfer)", VK_API_VERSION_1_3, WORK_FILL},
    {"H: + vkCmdPipelineBarrier", VK_API_VERSION_1_3, WORK_BARRIER},
    {"I: + vkCmdPipelineBarrier, HOST stage", VK_API_VERSION_1_3, WORK_BARRIER_HOST},
    {"J: probe loader persis (no-coherent memory)", VK_API_VERSION_1_3, WORK_AS_LOADER},
    {"L: + pEnabledFeatures kosong", VK_API_VERSION_1_3, WORK_FEATURES},
    {"K: instance kedua di anak", VK_API_VERSION_1_3, WORK_SECOND_INST},
    {"M: image+memori+barrier layout", VK_API_VERSION_1_3, WORK_IMAGE_BARRIER},
    {"N: sama, dengan TRANSFER_DST", VK_API_VERSION_1_3, WORK_IMAGE_XFER},
    {"O: image + DEVICE_LOCAL + barrier", VK_API_VERSION_1_3, WORK_IMAGE_LOCAL},
    {"P: DEPTH image + DEVICE_LOCAL", VK_API_VERSION_1_3, WORK_DEPTH},
    {"Q: physical device milik anak sendiri", VK_API_VERSION_1_3, WORK_REENUM},
};

VkInstance make_instance(uint32_t api_version)
{
    VkApplicationInfo app;
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "vk_intel_diag";
    app.apiVersion = api_version;

    VkInstanceCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS) return VK_NULL_HANDLE;
    return instance;
}

int run_case(const Case &c, VkPhysicalDevice physical, const char *device_name)
{
    VkInstance instance = make_instance(c.api_version);
    if (instance == VK_NULL_HANDLE) {
        printf("      vkCreateInstance(api %u.%u) gagal\n", VK_VERSION_MAJOR(c.api_version),
               VK_VERSION_MINOR(c.api_version));
        return 2;
    }

    // Every other case is handed a VkPhysicalDevice handle created in the parent, before the fork. The
    // loader's probe_child does the opposite: it creates its own instance and enumerates its own handles.
    // Q asks that question directly, because a handle that crosses a fork is a much less common path than
    // one an application takes, and it is the one difference left between the probe (which dies on submit)
    // and case J (which survives).
    if (c.work == WORK_REENUM) {
        uint32_t ndev = 0;
        vkEnumeratePhysicalDevices(instance, &ndev, nullptr);
        if (ndev) {
            std::vector<VkPhysicalDevice> devs(ndev);
            vkEnumeratePhysicalDevices(instance, &ndev, devs.data());
            physical = devs[0];
            printf("      device di-enumerate ulang di anak; ");
        }
    }

    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, nullptr);
    if (!queue_count) {
        printf("      tidak ada queue family\n");
        return 2;
    }
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof qci);
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof dci);
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    // The loader passes a zeroed VkPhysicalDeviceFeatures; an all-zero feature set is the same as none, but
    // it is a pointer the driver has to read, and that is a difference worth ruling out rather than assuming.
    VkPhysicalDeviceFeatures zeroed_features;
    memset(&zeroed_features, 0, sizeof zeroed_features);
    if (c.work == WORK_FEATURES) dci.pEnabledFeatures = &zeroed_features;
    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(physical, &dci, nullptr, &device) != VK_SUCCESS) {
        printf("      vkCreateDevice gagal\n");
        return 2;
    }
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, 0, 0, &queue);

    // One command buffer, recorded once. Nothing is allocated from device memory, so a crash here cannot be
    // blamed on memory: it is the submit path itself.
    VkBuffer transfer_buffer = VK_NULL_HANDLE; // WORK_FILL's buffer, kept alive across the recording

    VkCommandPoolCreateInfo pci;
    memset(&pci, 0, sizeof pci);
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(device, &pci, nullptr, &pool);

    VkCommandBufferAllocateInfo aci;
    memset(&aci, 0, sizeof aci);
    aci.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    aci.commandPool = pool;
    aci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    aci.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &aci, &cmd);

    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    if (c.work != WORK_FILL && c.work != WORK_EMPTY && c.work != WORK_MEMORY) {
        VkMemoryBarrier barrier;
        memset(&barrier, 0, sizeof barrier);
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        const VkPipelineStageFlags src =
            (c.work == WORK_BARRIER) ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_HOST_BIT;
        vkCmdPipelineBarrier(cmd, src, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        printf("barrier(%s) direkam; ", c.work == WORK_BARRIER ? "top-of-pipe" : "host");
    }
    if (c.work == WORK_DYNAMIC) {
        // Enough to make the driver build a render pass on the fly: begin rendering on a 16x16 attachment and
        // end it. No image is bound, because there is no memory allocated -- if this gets as far as the
        // submit, the driver has done the work it would do for a real pass.
        VkRenderingAttachmentInfo color;
        memset(&color, 0, sizeof color);
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color.float32[3] = 1.0f;

        VkRenderingInfo ri;
        memset(&ri, 0, sizeof ri);
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent.width = 16;
        ri.renderArea.extent.height = 16;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdEndRendering(cmd);
    }
    // Build up state before the submit, one case per call. Each step prints what happened, so the answer is
    // "this call" rather than "somewhere in here".
    if (c.work >= WORK_MEMORY) {
        VkBufferCreateInfo bci;
        memset(&bci, 0, sizeof bci);
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = 4096;
        // The transfer case uses the usage bits the loader's own probe uses, because that is the one that
        // crashes: a fill is a transfer operation, and a vertex buffer cannot be filled.
        bci.usage = (c.work == WORK_FILL) ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer buffer = VK_NULL_HANDLE;
        bool bound = false;
        if (vkCreateBuffer(device, &bci, nullptr, &buffer) != VK_SUCCESS) {
            printf("vkCreateBuffer gagal; ");
        } else {
            VkMemoryRequirements req;
            memset(&req, 0, sizeof req);
            vkGetBufferMemoryRequirements(device, buffer, &req);
            uint32_t type = UINT32_MAX;
            VkPhysicalDeviceMemoryProperties mp;
            vkGetPhysicalDeviceMemoryProperties(physical, &mp);
            // Two rules, on purpose. Most cases want coherent memory because that is what the loader's own
            // allocations use; WORK_AS_LOADER reproduces the probe's weaker rule (HOST_VISIBLE only) so the
            // difference between "survives" and "crashes" can be pinned to this choice rather than to
            // everything else that differs.
            const bool need_coherent = (c.work != WORK_AS_LOADER);
            for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
                const VkMemoryPropertyFlags flags = mp.memoryTypes[i].propertyFlags;
                if (!(req.memoryTypeBits & (1u << i))) continue;
                if (!(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
                if (need_coherent && !(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) continue;
                type = i;
                break;
            }
            if (type == UINT32_MAX) {
                printf("tidak ada memory type host-visible; ");
            } else {
                VkMemoryAllocateInfo mai;
                memset(&mai, 0, sizeof mai);
                mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                mai.allocationSize = req.size;
                mai.memoryTypeIndex = type;
                VkDeviceMemory memory = VK_NULL_HANDLE;
                if (vkAllocateMemory(device, &mai, nullptr, &memory) != VK_SUCCESS) {
                    printf("vkAllocateMemory gagal; ");
                } else {
                    vkBindBufferMemory(device, buffer, memory, 0);
                    bound = true;
                    printf("memori ok; ");
                }
            }
        }

        // Recorded here, not earlier. The first version of this case recorded the fill *before* the buffer
        // existed, so it filled VK_NULL_HANDLE: undefined behaviour that crashed every driver -- Intel and
        // llvmpipe alike -- and therefore told us nothing about transfer commands at all. A fill on a buffer
        // with no memory bound is equally invalid, so both conditions are checked before recording.
        if (c.work == WORK_FILL) {
            transfer_buffer = buffer;
            if (buffer != VK_NULL_HANDLE && bound) {
                vkCmdFillBuffer(cmd, buffer, 0, 256, 0xDEADBEEFu);
                printf("vkCmdFillBuffer direkam; ");
            } else {
                printf("buffer tanpa memori, fill dilewati (bukan crash); ");
            }
        }

        if (c.work == WORK_IMAGE_BARRIER || c.work == WORK_IMAGE_XFER || c.work == WORK_IMAGE_LOCAL ||
            c.work == WORK_DEPTH) {
            // The combination that is actually missing from the tests above: an image that has memory bound
            // and a layout transition recorded and submitted. gfx::create_color_target() does exactly this,
            // and that is where A0 dies once dynamic rendering is enabled.
            VkImageCreateInfo ici2;
            memset(&ici2, 0, sizeof ici2);
            ici2.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            ici2.imageType = VK_IMAGE_TYPE_2D;
            const bool depth = (c.work == WORK_DEPTH);
            ici2.format = depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_B8G8R8A8_UNORM;
            ici2.extent.width = 64;
            ici2.extent.height = 48;
            ici2.extent.depth = 1;
            ici2.mipLevels = 1;
            ici2.arrayLayers = 1;
            ici2.samples = VK_SAMPLE_COUNT_1_BIT;
            ici2.tiling = VK_IMAGE_TILING_OPTIMAL;
            ici2.usage =
                depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                      : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
            if (c.work == WORK_IMAGE_XFER || c.work == WORK_IMAGE_LOCAL)
                ici2.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if (depth) ici2.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            ici2.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            ici2.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImage image = VK_NULL_HANDLE;
            if (vkCreateImage(device, &ici2, nullptr, &image) != VK_SUCCESS) {
                printf("vkCreateImage gagal; ");
            } else {
                VkImageViewCreateInfo view_ci;
                memset(&view_ci, 0, sizeof view_ci);
                view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                view_ci.image = image;
                view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                view_ci.format = ici2.format;
                view_ci.subresourceRange.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
                view_ci.subresourceRange.levelCount = 1;
                view_ci.subresourceRange.layerCount = 1;
                VkImageView view = VK_NULL_HANDLE;
                if (vkCreateImageView(device, &view_ci, nullptr, &view) != VK_SUCCESS) {
                    printf("vkCreateImageView gagal; ");
                } else {
                    VkMemoryRequirements ireq;
                    memset(&ireq, 0, sizeof ireq);
                    vkGetImageMemoryRequirements(device, image, &ireq);
                    uint32_t itype = UINT32_MAX;
                    VkPhysicalDeviceMemoryProperties imp;
                    vkGetPhysicalDeviceMemoryProperties(physical, &imp);
                    // DEVICE_LOCAL for case O: what the loader uses for render targets, which on an
                    // integrated GPU is shared system memory reached through a different allocator.
                    const VkMemoryPropertyFlags iwant =
                        (c.work == WORK_IMAGE_LOCAL || c.work == WORK_DEPTH)
                            ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
                            : (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                    for (uint32_t i = 0; i < imp.memoryTypeCount; i++) {
                        if ((ireq.memoryTypeBits & (1u << i)) && (imp.memoryTypes[i].propertyFlags & iwant) == iwant) {
                            itype = i;
                            break;
                        }
                    }
                    VkDeviceMemory imemory = VK_NULL_HANDLE;
                    if (itype == UINT32_MAX) {
                        printf("image tanpa memory type; ");
                    } else {
                        VkMemoryAllocateInfo imai;
                        memset(&imai, 0, sizeof imai);
                        imai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
                        imai.allocationSize = ireq.size;
                        imai.memoryTypeIndex = itype;
                        if (vkAllocateMemory(device, &imai, nullptr, &imemory) != VK_SUCCESS) {
                            printf("vkAllocateMemory(image) gagal; ");
                        } else {
                            vkBindImageMemory(device, image, imemory, 0);
                            // The transition the loader records when it makes a target.
                            VkImageMemoryBarrier imb;
                            memset(&imb, 0, sizeof imb);
                            imb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                            imb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                            imb.newLayout =
                                depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                      : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                            imb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                            imb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                            imb.image = image;
                            imb.subresourceRange.aspectMask =
                                depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
                            imb.subresourceRange.levelCount = 1;
                            imb.subresourceRange.layerCount = 1;
                            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
                                                 nullptr, 1, &imb);
                            printf("image+memori+barrier direkam; ");
                        }
                    }
                }
            }
        }

        if (c.work == WORK_IMAGE || c.work == WORK_DESCRIPTORS) {
            VkImageCreateInfo image_ci;
            memset(&image_ci, 0, sizeof image_ci);
            image_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
            image_ci.imageType = VK_IMAGE_TYPE_2D;
            image_ci.format = VK_FORMAT_B8G8R8A8_UNORM;
            image_ci.extent.width = 16;
            image_ci.extent.height = 16;
            image_ci.extent.depth = 1;
            image_ci.mipLevels = 1;
            image_ci.arrayLayers = 1;
            image_ci.samples = VK_SAMPLE_COUNT_1_BIT;
            image_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
            image_ci.usage =
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            image_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            image_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImage image = VK_NULL_HANDLE;
            if (vkCreateImage(device, &image_ci, nullptr, &image) != VK_SUCCESS) {
                printf("vkCreateImage gagal; ");
            } else {
                VkImageViewCreateInfo view_ci;
                memset(&view_ci, 0, sizeof view_ci);
                view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                view_ci.image = image;
                view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                view_ci.format = image_ci.format;
                view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                view_ci.subresourceRange.levelCount = 1;
                view_ci.subresourceRange.layerCount = 1;
                VkImageView view = VK_NULL_HANDLE;
                if (vkCreateImageView(device, &view_ci, nullptr, &view) != VK_SUCCESS) {
                    printf("vkCreateImageView gagal; ");
                } else {
                    printf("image+view ok; ");
                    vkDestroyImageView(device, view, nullptr);
                }
                vkDestroyImage(device, image, nullptr);
            }
        }

        if (c.work == WORK_DESCRIPTORS) {
            // The suspect, asked on its own: a 32-bit process has been seen to die inside
            // vkAllocateDescriptorSets, and a draw reading an unwritten descriptor dies later -- so a crash
            // inside the submit is exactly what "the descriptor was never written" would look like.
            VkDescriptorSetLayoutBinding binding;
            memset(&binding, 0, sizeof binding);
            binding.binding = 0;
            binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            binding.descriptorCount = 1;
            binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo dsl_ci;
            memset(&dsl_ci, 0, sizeof dsl_ci);
            dsl_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            dsl_ci.bindingCount = 1;
            dsl_ci.pBindings = &binding;
            VkDescriptorSetLayout layout = VK_NULL_HANDLE;
            if (vkCreateDescriptorSetLayout(device, &dsl_ci, nullptr, &layout) != VK_SUCCESS) {
                printf("vkCreateDescriptorSetLayout gagal; ");
            } else {
                VkDescriptorPoolSize pool_size;
                pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                pool_size.descriptorCount = 1;
                VkDescriptorPoolCreateInfo pool_ci;
                memset(&pool_ci, 0, sizeof pool_ci);
                pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
                pool_ci.maxSets = 1;
                pool_ci.poolSizeCount = 1;
                pool_ci.pPoolSizes = &pool_size;
                VkDescriptorPool pool = VK_NULL_HANDLE;
                if (vkCreateDescriptorPool(device, &pool_ci, nullptr, &pool) != VK_SUCCESS) {
                    printf("vkCreateDescriptorPool gagal; ");
                } else {
                    VkDescriptorSetAllocateInfo dsai;
                    memset(&dsai, 0, sizeof dsai);
                    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                    dsai.descriptorPool = pool;
                    dsai.descriptorSetCount = 1;
                    dsai.pSetLayouts = &layout;
                    VkDescriptorSet set = VK_NULL_HANDLE;
                    printf("vkAllocateDescriptorSets=%d; ", (int)vkAllocateDescriptorSets(device, &dsai, &set));
                }
            }
        }
    }
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    const VkResult submit = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    const VkResult waited = vkQueueWaitIdle(queue);

    printf("      vkQueueSubmit=%d vkQueueWaitIdle=%d pada '%s'\n", (int)submit, (int)waited, device_name);
    return submit == VK_SUCCESS ? 0 : 3;
}

// One case in a child: the answer is "it survived" or "it died with signal N".
int run_in_child(const Case &c, VkPhysicalDevice physical, const char *device_name)
{
    fflush(nullptr);
    const pid_t pid = fork();
    if (pid == 0) {
        // gfx::init() creates the instance in the parent and *then* forks, so the child makes a second one.
        // A driver that cannot take two instances in one process would look exactly like "dies on submit",
        // which is why it is asked directly.
        if (c.work == WORK_SECOND_INST) {
            VkInstance first = make_instance(c.api_version);
            (void)first;
        }
        _exit(run_case(c, physical, device_name));
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return -1;
    if (WIFSIGNALED(status)) {
        printf("      MATI dengan signal %d\n", WTERMSIG(status));
        return -2;
    }
    return WEXITSTATUS(status);
}

} // namespace

int main(void)
{
    VkInstance instance = make_instance(VK_API_VERSION_1_3);
    if (instance == VK_NULL_HANDLE) {
        printf("vulkan: tidak bisa membuat instance\n");
        return 0;
    }
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (!count) {
        printf("vulkan: tidak ada device\n");
        return 0;
    }
    VkPhysicalDevice *devices = (VkPhysicalDevice *)malloc(count * sizeof *devices);
    vkEnumeratePhysicalDevices(instance, &count, devices);

    printf("vk_intel_diag -- 32-bit, %u device\n\n", count);
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devices[i], &p);
        printf("  %s (%s)\n", p.deviceName,
               p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? "software" : "hardware");
        for (const Case &c : kCases) {
            printf("    %-36s ", c.name);
            fflush(stdout);
            const int rc = run_in_child(c, devices[i], p.deviceName);
            if (rc == -2) printf("      -> crash (lihat baris di atas)\n");
            else if (rc == 0) printf("      -> selamat\n");
            else printf("      -> gagal tanpa crash (kode %d)\n", rc);
        }
        printf("\n");
    }
    return 0;
}