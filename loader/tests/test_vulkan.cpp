// Does Vulkan actually work inside a 32-bit process, and which features can we rely on?
//
// This is the check that decides whether the plan's graphics phase is real: the loader must be an
// i386 process (the XBE is x86-32 with absolute addresses), so it talks to a 32-bit Vulkan loader and
// a 32-bit ICD. If that combination does not enumerate a device, the whole Vulkan phase needs the
// "64-bit renderer daemon over shared memory" fallback instead.
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail;

static void check(bool cond, const char *what)
{
    printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) g_fail++;
}

static const char *result_string(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    default: return "other";
    }
}

int main()
{
    printf("BLACK loader -- Vulkan probe (i386)\n");
    printf("  sizeof(void*) = %zu\n", sizeof(void *));
    static_assert(sizeof(void *) == 4, "this probe is meant to run as a 32-bit binary");

    // --- instance -------------------------------------------------------
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "black";
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    VkInstance inst = nullptr;
    VkResult r = vkCreateInstance(&ici, nullptr, &inst);
    if (r != VK_SUCCESS) {
        printf("  FAIL vkCreateInstance = %s\n", result_string(r));
        return 1;
    }
    check(true, "vkCreateInstance");

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    check(n > 0, "at least one physical device");

    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(inst, &n, devices.data());

    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(devices[i], &p);
        printf("\n  device %u: %s\n", i, p.deviceName);
        printf("    api %u.%u.%u  driverType %d  discrete %u\n",
               VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
               VK_VERSION_PATCH(p.apiVersion), p.deviceType, p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
    }

    // --- the features the port's Vulkan backend actually depends on -------
    //
    // The two that matter most are both about reading a depth buffer as a texture: the game's blur
    // and depth-of-field passes do exactly that, and on D3D9 it was the INTZ driver extension
    // (d3d/host.h:29). Vulkan has no equivalent extension, so the format feature bits below are the
    // real question.
    VkPhysicalDevice chosen = devices[0];
    VkPhysicalDeviceFeatures feats{};
    vkGetPhysicalDeviceFeatures(chosen, &feats);
    check(feats.samplerAnisotropy, "samplerAnisotropy");
    check(feats.independentBlend, "independentBlend");
    check(feats.geometryShader == VK_TRUE || true, "geometryShader (informational)");
    check(feats.fragmentStoresAndAtomics || true, "fragmentStoresAndAtomics (informational)");

    VkFormatProperties fp32{};
    vkGetPhysicalDeviceFormatProperties(chosen, VK_FORMAT_D32_SFLOAT, &fp32);
    bool depth_stencil = fp32.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    bool depth_sampled = fp32.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    bool depth_linear = fp32.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    check(depth_stencil, "D32_SFLOAT usable as depth-stencil attachment (the D3D9 INTZ stand-in)");
    check(depth_sampled, "D32_SFLOAT usable as a sampled image");
    check(depth_linear, "D32_SFLOAT linear filtering when sampled");

    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(chosen, VK_FORMAT_R8G8B8A8_UNORM, &fp);
    check(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT,
          "R8G8B8A8_UNORM as colour attachment (stands in for A8R8G8B8)");
    check(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
          "R8G8B8A8_UNORM sampled");
    check(fp.optimalTilingFeatures & (VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT),
          "R8G8B8A8_UNORM linear filtering");

    // BC1/BC3 stand in for the DXT1/DXT5 textures the game ships (d3d/texture.cpp host_format()).
    VkFormatProperties bc1{}, bc3{};
    vkGetPhysicalDeviceFormatProperties(chosen, VK_FORMAT_BC1_RGB_UNORM_BLOCK, &bc1);
    vkGetPhysicalDeviceFormatProperties(chosen, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, &bc3);
    check(bc1.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, "BC1 sampled (DXT1)");
    check(bc3.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, "BC1_RGBA sampled (DXT1 with alpha)");

    VkFormatProperties bc2{};
    vkGetPhysicalDeviceFormatProperties(chosen, VK_FORMAT_BC2_UNORM_BLOCK, &bc2);
    check(bc2.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, "BC2 sampled (DXT3)");

    // 16 attributes is the hard ceiling the port already sits on: draw.cpp sends up to 16 float4
    // varyings (draw.cpp:512, draw.cpp:1017-1026).
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(chosen, &props);
    check(props.limits.maxVertexInputAttributes >= 16,
          "at least 16 vertex input attributes");
    printf("    maxVertexInputAttributes = %u\n", props.limits.maxVertexInputAttributes);
    check(props.limits.maxImageDimension2D >= 4096, "at least 4096px 2D images (render scale 3 = 2560x1440)");
    printf("    maxImageDimension2D = %u\n", props.limits.maxImageDimension2D);

    vkDestroyInstance(inst, nullptr);

    printf("\n%s\n", g_fail ? "FAIL: some required Vulkan features are missing"
                             : "Vulkan is usable from a 32-bit process");
    return g_fail ? 1 : 0;
}