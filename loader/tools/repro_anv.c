// Minimal reproducer for an ANV (Intel Vulkan) crash in
// anv_cmd_buffer_record_end_submit (anv_batch_chain.c:549), SIGSEGV on vkQueueSubmit.
//
// This is the stripped-down form of vk_probe32 from blacklinux. Everything above the crash
// (the window, the device probe loop, the fork-per-device isolation, the swapchain) is removed.
// What is left is the shortest sequence that reaches the faulting call, and nothing else.
//
// If this reproduces for you, the bug is in the driver and is not specific to our loader.
//
//   cc -m32 repro_anv.c -lvulkan -o repro_anv32
//   cc -m64 repro_anv.c -lvulkan -o repro_anv64
//
// Reported against: Mesa 25.2.8, Intel(R) Graphics (ADL GT2), Ubuntu 24.04 (noble).
// Note for the reproducer: unlike vkcube, this enables no features and no device extensions.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

#define CHK(expr)                                                                                \
    do {                                                                                         \
        VkResult r_ = (expr);                                                                    \
        if (r_ != VK_SUCCESS) {                                                                  \
            fprintf(stderr, "%s failed: %d\n", #expr, (int)r_);                                  \
            exit(1);                                                                             \
        }                                                                                        \
    } while (0)

static uint32_t find_graphics_queue(VkPhysicalDevice pd, VkPhysicalDeviceProperties *props)
{
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, NULL);
    VkQueueFamilyProperties *q = calloc(n, sizeof *q);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, q);
    uint32_t found = UINT32_MAX;
    for (uint32_t i = 0; i < n; i++) {
        if (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            found = i;
            printf("queue family %u: flags=0x%x count=%u\n", i, q[i].queueFlags, q[i].queueCount);
            break;
        }
    }
    free(q);
    (void)props;
    return found;
}

int main(int argc, char **argv)
{
    const char *want = (argc > 1) ? argv[1] : NULL;

    VkApplicationInfo app;
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof ici);
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance inst;
    CHK(vkCreateInstance(&ici, NULL, &inst));

    uint32_t pdn = 0;
    CHK(vkEnumeratePhysicalDevices(inst, &pdn, NULL));
    VkPhysicalDevice *pds = calloc(pdn, sizeof *pds);
    CHK(vkEnumeratePhysicalDevices(inst, &pdn, pds));

    for (uint32_t i = 0; i < pdn; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pds[i], &props);
        printf("device %u: %s\n", i, props.deviceName);
        if (want && !strstr(props.deviceName, want)) continue;

        uint32_t fam = find_graphics_queue(pds[i], &props);
        if (fam == UINT32_MAX) continue;

        const float prio = 1.0f;
        VkDeviceQueueCreateInfo qci;
        memset(&qci, 0, sizeof qci);
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = fam;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;

        VkPhysicalDeviceFeatures feats; // all zero, as in our loader
        memset(&feats, 0, sizeof feats);

        VkDeviceCreateInfo dci;
        memset(&dci, 0, sizeof dci);
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.pEnabledFeatures = &feats;

        VkDevice dev;
        CHK(vkCreateDevice(pds[i], &dci, NULL, &dev));
        printf("  device created, submitting...\n");
        fflush(stdout);

        VkQueue queue;
        vkGetDeviceQueue(dev, fam, 0, &queue);

        // A 256-byte buffer, and one barrier. Both exist so the command buffer is not empty --
        // an empty submit would not prove the driver can execute anything.
        VkBufferCreateInfo bci;
        memset(&bci, 0, sizeof bci);
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = 256;
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VkBuffer buf;
        CHK(vkCreateBuffer(dev, &bci, NULL, &buf));

        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, buf, &req);

        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(pds[i], &mp);
        uint32_t type = UINT32_MAX;
        for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
            VkMemoryPropertyFlags f = mp.memoryTypes[t].propertyFlags;
            if (!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
            type = t; // prefer HOST_COHERENT, fall back to HOST_VISIBLE
            if (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) break;
        }
        if (type == UINT32_MAX) { fprintf(stderr, "no host-visible memory\n"); return 1; }

        VkMemoryAllocateInfo mai;
        memset(&mai, 0, sizeof mai);
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = type;
        VkDeviceMemory mem;
        CHK(vkAllocateMemory(dev, &mai, NULL, &mem));
        CHK(vkBindBufferMemory(dev, buf, mem, 0));

        VkCommandPoolCreateInfo pci;
        memset(&pci, 0, sizeof pci);
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = fam;
        VkCommandPool pool;
        CHK(vkCreateCommandPool(dev, &pci, NULL, &pool));

        VkCommandBufferAllocateInfo cai;
        memset(&cai, 0, sizeof cai);
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cmd;
        CHK(vkAllocateCommandBuffers(dev, &cai, &cmd));

        VkCommandBufferBeginInfo bi;
        memset(&bi, 0, sizeof bi);
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CHK(vkBeginCommandBuffer(cmd, &bi));

        VkMemoryBarrier barrier;
        memset(&barrier, 0, sizeof barrier);
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0,
                             NULL, 0, NULL);

        VkSubmitInfo si;
        memset(&si, 0, sizeof si);
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;

        printf("  vkQueueSubmit...\n");
        fflush(stdout);
        CHK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE)); // <-- SIGSEGV inside ANV
        CHK(vkQueueWaitIdle(queue));
        printf("  survived\n");
    }

    printf("no crash\n");
    return 0;
}
