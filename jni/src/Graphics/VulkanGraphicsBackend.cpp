#include "Graphics/VulkanGraphicsBackend.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <imgui.h>
#include <utility>

namespace {

constexpr uint32_t kMaxTextures = 1024;
constexpr VkFormat kTextureFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkImageSubresourceRange kWholeColorImage = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

// Precompiled SPIR-V UI shaders (pos, uv, color -> premultiplied RGBA with push-constant scale/translate).
constexpr uint32_t kUiVertSpv[] = {
    0x07230203, 0x00010000, 0x000d000a, 0x0000002e, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x000b000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000d, 0x00000012, 0x00000027,
    0x00000028, 0x0000002a, 0x0000002c, 0x00050048, 0x0000000b, 0x00000000, 0x0000000b, 0x00000000,
    0x00050048, 0x0000000b, 0x00000001, 0x0000000b, 0x00000001, 0x00050048, 0x0000000b, 0x00000002,
    0x0000000b, 0x00000003, 0x00050048, 0x0000000b, 0x00000003, 0x0000000b, 0x00000004, 0x00030047,
    0x0000000b, 0x00000002, 0x00040047, 0x00000012, 0x0000001e, 0x00000000, 0x00050048, 0x00000014,
    0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x00000014, 0x00000001, 0x00000023, 0x00000008,
    0x00030047, 0x00000014, 0x00000002, 0x00040047, 0x00000027, 0x0000001e, 0x00000000, 0x00040047,
    0x00000028, 0x0000001e, 0x00000001, 0x00040047, 0x0000002a, 0x0000001e, 0x00000001, 0x00040047,
    0x0000002c, 0x0000001e, 0x00000002, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
    0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040015,
    0x00000008, 0x00000020, 0x00000000, 0x0004002b, 0x00000008, 0x00000009, 0x00000001, 0x0004001c,
    0x0000000a, 0x00000006, 0x00000009, 0x0006001e, 0x0000000b, 0x00000007, 0x00000006, 0x0000000a,
    0x0000000a, 0x00040020, 0x0000000c, 0x00000003, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d,
    0x00000003, 0x00040015, 0x0000000e, 0x00000020, 0x00000001, 0x0004002b, 0x0000000e, 0x0000000f,
    0x00000000, 0x00040017, 0x00000010, 0x00000006, 0x00000002, 0x00040020, 0x00000011, 0x00000001,
    0x00000010, 0x0004003b, 0x00000011, 0x00000012, 0x00000001, 0x0004001e, 0x00000014, 0x00000010,
    0x00000010, 0x00040020, 0x00000015, 0x00000009, 0x00000014, 0x0004003b, 0x00000015, 0x00000016,
    0x00000009, 0x00040020, 0x00000017, 0x00000009, 0x00000010, 0x0004002b, 0x0000000e, 0x0000001b,
    0x00000001, 0x0004002b, 0x00000006, 0x0000001f, 0x00000000, 0x0004002b, 0x00000006, 0x00000020,
    0x3f800000, 0x00040020, 0x00000024, 0x00000003, 0x00000007, 0x00040020, 0x00000026, 0x00000003,
    0x00000010, 0x0004003b, 0x00000026, 0x00000027, 0x00000003, 0x0004003b, 0x00000011, 0x00000028,
    0x00000001, 0x0004003b, 0x00000024, 0x0000002a, 0x00000003, 0x00040020, 0x0000002b, 0x00000001,
    0x00000007, 0x0004003b, 0x0000002b, 0x0000002c, 0x00000001, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x00000010, 0x00000013, 0x00000012,
    0x00050041, 0x00000017, 0x00000018, 0x00000016, 0x0000000f, 0x0004003d, 0x00000010, 0x00000019,
    0x00000018, 0x00050041, 0x00000017, 0x0000001c, 0x00000016, 0x0000001b, 0x0004003d, 0x00000010,
    0x0000001d, 0x0000001c, 0x0008000c, 0x00000010, 0x0000001e, 0x00000001, 0x00000032, 0x00000013,
    0x00000019, 0x0000001d, 0x00050051, 0x00000006, 0x00000021, 0x0000001e, 0x00000000, 0x00050051,
    0x00000006, 0x00000022, 0x0000001e, 0x00000001, 0x00070050, 0x00000007, 0x00000023, 0x00000021,
    0x00000022, 0x0000001f, 0x00000020, 0x00050041, 0x00000024, 0x00000025, 0x0000000d, 0x0000000f,
    0x0003003e, 0x00000025, 0x00000023, 0x0004003d, 0x00000010, 0x00000029, 0x00000028, 0x0003003e,
    0x00000027, 0x00000029, 0x0004003d, 0x00000007, 0x0000002d, 0x0000002c, 0x0003003e, 0x0000002a,
    0x0000002d, 0x000100fd, 0x00010038
};

constexpr uint32_t kUiFragSpv[] = {
    0x07230203, 0x00010000, 0x000d000a, 0x00000018, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0008000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x0000000b, 0x00000014,
    0x00030010, 0x00000004, 0x00000007, 0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00040047,
    0x0000000b, 0x0000001e, 0x00000001, 0x00040047, 0x00000010, 0x00000022, 0x00000000, 0x00040047,
    0x00000010, 0x00000021, 0x00000000, 0x00040047, 0x00000014, 0x0000001e, 0x00000000, 0x00020013,
    0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017,
    0x00000007, 0x00000006, 0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b,
    0x00000008, 0x00000009, 0x00000003, 0x00040020, 0x0000000a, 0x00000001, 0x00000007, 0x0004003b,
    0x0000000a, 0x0000000b, 0x00000001, 0x00090019, 0x0000000d, 0x00000006, 0x00000001, 0x00000000,
    0x00000000, 0x00000000, 0x00000001, 0x00000000, 0x0003001b, 0x0000000e, 0x0000000d, 0x00040020,
    0x0000000f, 0x00000000, 0x0000000e, 0x0004003b, 0x0000000f, 0x00000010, 0x00000000, 0x00040017,
    0x00000012, 0x00000006, 0x00000002, 0x00040020, 0x00000013, 0x00000001, 0x00000012, 0x0004003b,
    0x00000013, 0x00000014, 0x00000001, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003,
    0x000200f8, 0x00000005, 0x0004003d, 0x00000007, 0x0000000c, 0x0000000b, 0x0004003d, 0x0000000e,
    0x00000011, 0x00000010, 0x0004003d, 0x00000012, 0x00000015, 0x00000014, 0x00050057, 0x00000007,
    0x00000016, 0x00000011, 0x00000015, 0x00050085, 0x00000007, 0x00000017, 0x0000000c, 0x00000016,
    0x0003003e, 0x00000009, 0x00000017, 0x000100fd, 0x00010038
};

VkDeviceSize NextPowerOfTwo(VkDeviceSize value) {
    if (value <= 65536ULL) {
        return 65536ULL;
    }
    VkDeviceSize p = 65536ULL;
    while (p < value && p < (VkDeviceSize{1} << 62)) {
        p <<= 1;
    }
    return p;
}

bool CheckVk(const char* what, VkResult res) {
    if (res == VK_SUCCESS) {
        return true;
    }
    std::printf("[Vulkan] %s failed (VkResult=%d)\n", what, static_cast<int>(res));
    return false;
}

} // namespace

bool VulkanGraphicsBackend::InstanceApi::Load(PFN_vkGetInstanceProcAddr getProc, VkInstance inst) {
    const char* missing = nullptr;
    auto bind = [&](auto& slot, const char* name) {
        slot = reinterpret_cast<typename std::remove_reference<decltype(slot)>::type>(getProc(inst, name));
        if (!slot && !missing) {
            missing = name;
        }
    };

    bind(destroyInstance, "vkDestroyInstance");
    bind(enumeratePhysicalDevices, "vkEnumeratePhysicalDevices");
    bind(getPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties");
    bind(getPhysicalDeviceQueueFamilyProperties, "vkGetPhysicalDeviceQueueFamilyProperties");
    bind(getPhysicalDeviceMemoryProperties, "vkGetPhysicalDeviceMemoryProperties");
    bind(createDevice, "vkCreateDevice");
    bind(getDeviceProcAddr, "vkGetDeviceProcAddr");

    bind(createAndroidSurface, "vkCreateAndroidSurfaceKHR");
    bind(destroySurface, "vkDestroySurfaceKHR");
    bind(getPhysicalDeviceSurfaceSupport, "vkGetPhysicalDeviceSurfaceSupportKHR");
    bind(getPhysicalDeviceSurfaceCapabilities, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    bind(getPhysicalDeviceSurfaceFormats, "vkGetPhysicalDeviceSurfaceFormatsKHR");

    if (missing) {
        std::printf("[Vulkan] Missing instance proc: %s\n", missing);
        return false;
    }
    return true;
}

bool VulkanGraphicsBackend::DeviceApi::Load(PFN_vkGetDeviceProcAddr getProc, VkDevice dev) {
    const char* missing = nullptr;
    auto bind = [&](auto& slot, const char* name) {
        slot = reinterpret_cast<typename std::remove_reference<decltype(slot)>::type>(getProc(dev, name));
        if (!slot && !missing) {
            missing = name;
        }
    };

    bind(destroyDevice, "vkDestroyDevice");
    bind(getDeviceQueue, "vkGetDeviceQueue");
    bind(deviceWaitIdle, "vkDeviceWaitIdle");
    bind(queueSubmit, "vkQueueSubmit");
    bind(queueWaitIdle, "vkQueueWaitIdle");

    bind(createCommandPool, "vkCreateCommandPool");
    bind(destroyCommandPool, "vkDestroyCommandPool");
    bind(allocateCommandBuffers, "vkAllocateCommandBuffers");
    bind(beginCommandBuffer, "vkBeginCommandBuffer");
    bind(endCommandBuffer, "vkEndCommandBuffer");
    bind(createSemaphore, "vkCreateSemaphore");
    bind(destroySemaphore, "vkDestroySemaphore");
    bind(createFence, "vkCreateFence");
    bind(destroyFence, "vkDestroyFence");
    bind(resetFences, "vkResetFences");
    bind(waitForFences, "vkWaitForFences");

    bind(createDescriptorSetLayout, "vkCreateDescriptorSetLayout");
    bind(destroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout");
    bind(createPipelineLayout, "vkCreatePipelineLayout");
    bind(destroyPipelineLayout, "vkDestroyPipelineLayout");
    bind(createDescriptorPool, "vkCreateDescriptorPool");
    bind(destroyDescriptorPool, "vkDestroyDescriptorPool");
    bind(allocateDescriptorSets, "vkAllocateDescriptorSets");
    bind(freeDescriptorSets, "vkFreeDescriptorSets");
    bind(updateDescriptorSets, "vkUpdateDescriptorSets");
    bind(createRenderPass, "vkCreateRenderPass");
    bind(destroyRenderPass, "vkDestroyRenderPass");
    bind(createShaderModule, "vkCreateShaderModule");
    bind(destroyShaderModule, "vkDestroyShaderModule");
    bind(createGraphicsPipelines, "vkCreateGraphicsPipelines");
    bind(destroyPipeline, "vkDestroyPipeline");
    bind(createFramebuffer, "vkCreateFramebuffer");
    bind(destroyFramebuffer, "vkDestroyFramebuffer");

    bind(allocateMemory, "vkAllocateMemory");
    bind(freeMemory, "vkFreeMemory");
    bind(mapMemory, "vkMapMemory");
    bind(createBuffer, "vkCreateBuffer");
    bind(destroyBuffer, "vkDestroyBuffer");
    bind(getBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
    bind(bindBufferMemory, "vkBindBufferMemory");
    bind(createImage, "vkCreateImage");
    bind(destroyImage, "vkDestroyImage");
    bind(getImageMemoryRequirements, "vkGetImageMemoryRequirements");
    bind(bindImageMemory, "vkBindImageMemory");
    bind(createImageView, "vkCreateImageView");
    bind(destroyImageView, "vkDestroyImageView");
    bind(createSampler, "vkCreateSampler");
    bind(destroySampler, "vkDestroySampler");

    bind(cmdPipelineBarrier, "vkCmdPipelineBarrier");
    bind(cmdCopyBufferToImage, "vkCmdCopyBufferToImage");
    bind(cmdBeginRenderPass, "vkCmdBeginRenderPass");
    bind(cmdEndRenderPass, "vkCmdEndRenderPass");
    bind(cmdBindPipeline, "vkCmdBindPipeline");
    bind(cmdSetViewport, "vkCmdSetViewport");
    bind(cmdSetScissor, "vkCmdSetScissor");
    bind(cmdPushConstants, "vkCmdPushConstants");
    bind(cmdBindVertexBuffers, "vkCmdBindVertexBuffers");
    bind(cmdBindIndexBuffer, "vkCmdBindIndexBuffer");
    bind(cmdBindDescriptorSets, "vkCmdBindDescriptorSets");
    bind(cmdDrawIndexed, "vkCmdDrawIndexed");

    bind(createSwapchain, "vkCreateSwapchainKHR");
    bind(destroySwapchain, "vkDestroySwapchainKHR");
    bind(getSwapchainImages, "vkGetSwapchainImagesKHR");
    bind(acquireNextImage, "vkAcquireNextImageKHR");
    bind(queuePresent, "vkQueuePresentKHR");

    if (missing) {
        std::printf("[Vulkan] Missing device proc: %s\n", missing);
        return false;
    }
    return true;
}

bool VulkanGraphicsBackend::AttachSurface(ANativeWindow* window) {
    if (surface_ != VK_NULL_HANDLE && instApi_.destroySurface) {
        instApi_.destroySurface(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    boundWindow_ = nullptr;

    VkAndroidSurfaceCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    info.window = window;
    if (!CheckVk("vkCreateAndroidSurfaceKHR",
                 instApi_.createAndroidSurface(instance_, &info, nullptr, &surface_))) {
        return false;
    }
    boundWindow_ = window;
    return true;
}

bool VulkanGraphicsBackend::SelectPhysicalDevice() {
    uint32_t gpuCount = 0;
    if (!CheckVk("vkEnumeratePhysicalDevices",
                 instApi_.enumeratePhysicalDevices(instance_, &gpuCount, nullptr)) ||
        gpuCount == 0) {
        return false;
    }

    std::vector<VkPhysicalDevice> gpus(gpuCount);
    if (!CheckVk("vkEnumeratePhysicalDevices",
                 instApi_.enumeratePhysicalDevices(instance_, &gpuCount, gpus.data()))) {
        return false;
    }

    for (VkPhysicalDevice gpu : gpus) {
        uint32_t familyCount = 0;
        instApi_.getPhysicalDeviceQueueFamilyProperties(gpu, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        instApi_.getPhysicalDeviceQueueFamilyProperties(gpu, &familyCount, families.data());

        for (uint32_t fam = 0; fam < familyCount; ++fam) {
            if ((families[fam].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                continue;
            }
            VkBool32 supportsPresent = VK_FALSE;
            if (instApi_.getPhysicalDeviceSurfaceSupport(gpu, fam, surface_, &supportsPresent) == VK_SUCCESS &&
                supportsPresent == VK_TRUE) {
                physical_ = gpu;
                queueFamily_ = fam;
                return true;
            }
        }
    }

    std::printf("[Vulkan] No physical device supports graphics + Android surface present\n");
    return false;
}

bool VulkanGraphicsBackend::CreatePipeline() {
    auto createModule = [&](const uint32_t* code, size_t byteSize) -> VkShaderModule {
        VkShaderModuleCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = byteSize;
        info.pCode = code;
        VkShaderModule mod = VK_NULL_HANDLE;
        if (!CheckVk("vkCreateShaderModule", devApi_.createShaderModule(device_, &info, nullptr, &mod))) {
            return VK_NULL_HANDLE;
        }
        return mod;
    };

    VkShaderModule vertMod = createModule(kUiVertSpv, sizeof(kUiVertSpv));
    if (vertMod == VK_NULL_HANDLE) {
        return false;
    }
    VkShaderModule fragMod = createModule(kUiFragSpv, sizeof(kUiFragSpv));
    if (fragMod == VK_NULL_HANDLE) {
        devApi_.destroyShaderModule(device_, vertMod, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName = "main";

    const VkVertexInputBindingDescription binding{0, sizeof(ImDrawVert), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attrs[3] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ImDrawVert, pos)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ImDrawVert, uv)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(ImDrawVert, col)}
    };

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo msaa{};
    msaa.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Output premultiplied RGBA for transparent SurfaceFlinger composition.
    VkPipelineColorBlendAttachmentState blendAtt{};
    blendAtt.blendEnable = VK_TRUE;
    blendAtt.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAtt.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAtt.colorBlendOp = VK_BLEND_OP_ADD;
    blendAtt.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAtt.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAtt.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAtt;

    const VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynInfo{};
    dynInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynInfo.dynamicStateCount = 2;
    dynInfo.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo pipeInfo{};
    pipeInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeInfo.stageCount = 2;
    pipeInfo.pStages = stages;
    pipeInfo.pVertexInputState = &vertexInput;
    pipeInfo.pInputAssemblyState = &assembly;
    pipeInfo.pViewportState = &viewport;
    pipeInfo.pRasterizationState = &raster;
    pipeInfo.pMultisampleState = &msaa;
    pipeInfo.pColorBlendState = &blend;
    pipeInfo.pDynamicState = &dynInfo;
    pipeInfo.layout = pipelineLayout_;
    pipeInfo.renderPass = renderPass_;

    const VkResult res = devApi_.createGraphicsPipelines(
        device_, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &pipeline_);
    devApi_.destroyShaderModule(device_, vertMod, nullptr);
    devApi_.destroyShaderModule(device_, fragMod, nullptr);
    return CheckVk("vkCreateGraphicsPipelines", res);
}

bool VulkanGraphicsBackend::PrepareDeviceObjects() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily_;
    if (!CheckVk("vkCreateCommandPool", devApi_.createCommandPool(device_, &poolInfo, nullptr, &commandPool_))) {
        return false;
    }

    VkCommandBufferAllocateInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdInfo.commandPool = commandPool_;
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    if (!CheckVk("vkAllocateCommandBuffers", devApi_.allocateCommandBuffers(device_, &cmdInfo, &commandBuffer_))) {
        return false;
    }

    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (!CheckVk("vkCreateSemaphore", devApi_.createSemaphore(device_, &semInfo, nullptr, &imageAcquiredSem_)) ||
        !CheckVk("vkCreateSemaphore", devApi_.createSemaphore(device_, &semInfo, nullptr, &renderCompleteSem_))) {
        return false;
    }

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (!CheckVk("vkCreateFence", devApi_.createFence(device_, &fenceInfo, nullptr, &frameIdleFence_))) {
        return false;
    }

    VkDescriptorSetLayoutBinding binding{};
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo setLayoutInfo{};
    setLayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setLayoutInfo.bindingCount = 1;
    setLayoutInfo.pBindings = &binding;
    if (!CheckVk("vkCreateDescriptorSetLayout",
                 devApi_.createDescriptorSetLayout(device_, &setLayoutInfo, nullptr, &setLayout_))) {
        return false;
    }

    const VkPushConstantRange pushRange{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 4};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (!CheckVk("vkCreatePipelineLayout",
                 devApi_.createPipelineLayout(device_, &layoutInfo, nullptr, &pipelineLayout_))) {
        return false;
    }

    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures};
    VkDescriptorPoolCreateInfo descPoolInfo{};
    descPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descPoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    descPoolInfo.maxSets = kMaxTextures;
    descPoolInfo.poolSizeCount = 1;
    descPoolInfo.pPoolSizes = &poolSize;
    if (!CheckVk("vkCreateDescriptorPool",
                 devApi_.createDescriptorPool(device_, &descPoolInfo, nullptr, &descriptorPool_))) {
        return false;
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!CheckVk("vkCreateSampler", devApi_.createSampler(device_, &samplerInfo, nullptr, &sampler_))) {
        return false;
    }

    // Pick 8-bit UNORM surface format matching straight-alpha UI rendering.
    uint32_t fmtCount = 0;
    if (!CheckVk("vkGetPhysicalDeviceSurfaceFormatsKHR",
                 instApi_.getPhysicalDeviceSurfaceFormats(physical_, surface_, &fmtCount, nullptr)) ||
        fmtCount == 0) {
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(fmtCount);
    if (!CheckVk("vkGetPhysicalDeviceSurfaceFormatsKHR",
                 instApi_.getPhysicalDeviceSurfaceFormats(physical_, surface_, &fmtCount, formats.data()))) {
        return false;
    }

    surfaceFormat_.format = VK_FORMAT_UNDEFINED;
    const VkFormat preferred[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
    for (VkFormat want : preferred) {
        for (const VkSurfaceFormatKHR& f : formats) {
            if (f.format == want) {
                surfaceFormat_ = f;
                break;
            }
        }
        if (surfaceFormat_.format != VK_FORMAT_UNDEFINED) {
            break;
        }
    }
    if (surfaceFormat_.format == VK_FORMAT_UNDEFINED) {
        std::printf("[Vulkan] No 8-bit RGBA/BGRA UNORM surface format supported\n");
        return false;
    }

    VkAttachmentDescription colorAtt{};
    colorAtt.format = surfaceFormat_.format;
    colorAtt.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAtt.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    const VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &colorAtt;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies = &dep;
    if (!CheckVk("vkCreateRenderPass", devApi_.createRenderPass(device_, &rpInfo, nullptr, &renderPass_))) {
        return false;
    }

    return CreatePipeline();
}

VkImageView VulkanGraphicsBackend::CreateImageView(VkImage image, VkFormat format) const {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange = kWholeColorImage;
    VkImageView view = VK_NULL_HANDLE;
    if (!CheckVk("vkCreateImageView", devApi_.createImageView(device_, &info, nullptr, &view))) {
        return VK_NULL_HANDLE;
    }
    return view;
}

void VulkanGraphicsBackend::DestroySwapchainTarget(SwapchainTarget& target) const {
    for (VkFramebuffer fb : target.framebuffers) {
        if (fb != VK_NULL_HANDLE) {
            devApi_.destroyFramebuffer(device_, fb, nullptr);
        }
    }
    for (VkImageView view : target.views) {
        if (view != VK_NULL_HANDLE) {
            devApi_.destroyImageView(device_, view, nullptr);
        }
    }
    if (target.swapchain != VK_NULL_HANDLE) {
        devApi_.destroySwapchain(device_, target.swapchain, nullptr);
    }
    target = {};
}

bool VulkanGraphicsBackend::RebuildSwapchain() {
    if (!CheckVk("vkDeviceWaitIdle", devApi_.deviceWaitIdle(device_))) {
        return false;
    }

    VkSurfaceCapabilitiesKHR caps{};
    if (!CheckVk("vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
                 instApi_.getPhysicalDeviceSurfaceCapabilities(physical_, surface_, &caps))) {
        return false;
    }

    const VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0 || extent.height == 0 || extent.width == UINT32_MAX) {
        std::printf("[Vulkan] Surface reports invalid extent (%ux%u)\n", extent.width, extent.height);
        return false;
    }

    VkCompositeAlphaFlagBitsKHR compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(0);
    if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) {
        compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    } else if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) {
        compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    } else {
        std::printf("[Vulkan] Surface does not support transparent compositeAlpha\n");
        return false;
    }

    const bool supportsIdentity =
        (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0;
    const uint32_t maxImages = (caps.maxImageCount == 0) ? UINT32_MAX : caps.maxImageCount;

    SwapchainTarget previous = std::move(target_);
    target_ = {};

    VkSwapchainCreateInfoKHR scInfo{};
    scInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    scInfo.surface = surface_;
    scInfo.minImageCount = std::min(caps.minImageCount + 1U, maxImages);
    scInfo.imageFormat = surfaceFormat_.format;
    scInfo.imageColorSpace = surfaceFormat_.colorSpace;
    scInfo.imageExtent = extent;
    scInfo.imageArrayLayers = 1;
    scInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    scInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    scInfo.preTransform = supportsIdentity ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    scInfo.compositeAlpha = compositeAlpha;
    scInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    scInfo.clipped = VK_TRUE;
    scInfo.oldSwapchain = previous.swapchain;

    VkSwapchainKHR nextSwapchain = VK_NULL_HANDLE;
    const VkResult res = devApi_.createSwapchain(device_, &scInfo, nullptr, &nextSwapchain);
    DestroySwapchainTarget(previous);
    if (!CheckVk("vkCreateSwapchainKHR", res)) {
        return false;
    }

    target_.swapchain = nextSwapchain;
    target_.extent = extent;

    uint32_t imgCount = 0;
    if (!CheckVk("vkGetSwapchainImagesKHR",
                 devApi_.getSwapchainImages(device_, target_.swapchain, &imgCount, nullptr))) {
        return false;
    }
    std::vector<VkImage> images(imgCount);
    if (!CheckVk("vkGetSwapchainImagesKHR",
                 devApi_.getSwapchainImages(device_, target_.swapchain, &imgCount, images.data()))) {
        return false;
    }

    target_.views.reserve(imgCount);
    target_.framebuffers.reserve(imgCount);
    for (VkImage img : images) {
        VkImageView view = CreateImageView(img, surfaceFormat_.format);
        if (view == VK_NULL_HANDLE) {
            return false;
        }
        target_.views.push_back(view);

        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = renderPass_;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &target_.views.back();
        fbInfo.width = extent.width;
        fbInfo.height = extent.height;
        fbInfo.layers = 1;

        VkFramebuffer fb = VK_NULL_HANDLE;
        if (!CheckVk("vkCreateFramebuffer", devApi_.createFramebuffer(device_, &fbInfo, nullptr, &fb))) {
            return false;
        }
        target_.framebuffers.push_back(fb);
    }

    return true;
}

VkDeviceMemory VulkanGraphicsBackend::AllocateMemory(
    const VkMemoryRequirements& reqs,
    VkMemoryPropertyFlags flags) const
{
    for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i) {
        if ((reqs.memoryTypeBits & (1U << i)) == 0) {
            continue;
        }
        if ((memProps_.memoryTypes[i].propertyFlags & flags) != flags) {
            continue;
        }
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize = reqs.size;
        alloc.memoryTypeIndex = i;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (CheckVk("vkAllocateMemory", devApi_.allocateMemory(device_, &alloc, nullptr, &mem))) {
            return mem;
        }
        return VK_NULL_HANDLE;
    }
    std::printf("[Vulkan] No suitable memory type found (bits=0x%x flags=0x%x)\n",
                reqs.memoryTypeBits, flags);
    return VK_NULL_HANDLE;
}

bool VulkanGraphicsBackend::CreateBuffer(
    VkBufferUsageFlags usage,
    VkDeviceSize minSize,
    MappedBuffer& outBuf) const
{
    outBuf = {};
    outBuf.usage = usage;
    outBuf.capacity = NextPowerOfTwo(minSize);

    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = outBuf.capacity;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (!CheckVk("vkCreateBuffer", devApi_.createBuffer(device_, &info, nullptr, &outBuf.buffer))) {
        return false;
    }

    VkMemoryRequirements reqs{};
    devApi_.getBufferMemoryRequirements(device_, outBuf.buffer, &reqs);
    outBuf.memory = AllocateMemory(
        reqs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (outBuf.memory == VK_NULL_HANDLE) {
        DestroyBuffer(outBuf);
        return false;
    }

    if (!CheckVk("vkBindBufferMemory", devApi_.bindBufferMemory(device_, outBuf.buffer, outBuf.memory, 0))) {
        DestroyBuffer(outBuf);
        return false;
    }

    void* mapped = nullptr;
    if (!CheckVk("vkMapMemory", devApi_.mapMemory(device_, outBuf.memory, 0, VK_WHOLE_SIZE, 0, &mapped))) {
        DestroyBuffer(outBuf);
        return false;
    }
    outBuf.mapped = static_cast<uint8_t*>(mapped);
    return true;
}

void VulkanGraphicsBackend::DestroyBuffer(MappedBuffer& buf) const {
    if (buf.buffer != VK_NULL_HANDLE) {
        devApi_.destroyBuffer(device_, buf.buffer, nullptr);
        buf.buffer = VK_NULL_HANDLE;
    }
    if (buf.memory != VK_NULL_HANDLE) {
        devApi_.freeMemory(device_, buf.memory, nullptr);
        buf.memory = VK_NULL_HANDLE;
    }
    buf.capacity = 0;
    buf.mapped = nullptr;
}

bool VulkanGraphicsBackend::EnsureBufferCapacity(MappedBuffer& buf, VkDeviceSize needed) const {
    if (buf.capacity >= std::max<VkDeviceSize>(needed, 1ULL)) {
        return true;
    }
    MappedBuffer next{};
    if (!CreateBuffer(buf.usage, needed, next)) {
        return false;
    }
    DestroyBuffer(buf);
    buf = next;
    return true;
}

bool VulkanGraphicsBackend::CreateTexture(uint32_t width, uint32_t height, GpuTexture& outTex) const {
    outTex = {};

    VkImageCreateInfo imgInfo{};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = kTextureFormat;
    imgInfo.extent = {width, height, 1};
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    if (!CheckVk("vkCreateImage", devApi_.createImage(device_, &imgInfo, nullptr, &outTex.image))) {
        return false;
    }

    VkMemoryRequirements reqs{};
    devApi_.getImageMemoryRequirements(device_, outTex.image, &reqs);
    outTex.memory = AllocateMemory(reqs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (outTex.memory == VK_NULL_HANDLE ||
        !CheckVk("vkBindImageMemory", devApi_.bindImageMemory(device_, outTex.image, outTex.memory, 0))) {
        DestroyTexture(outTex);
        outTex = {};
        return false;
    }

    outTex.view = CreateImageView(outTex.image, kTextureFormat);
    if (outTex.view == VK_NULL_HANDLE) {
        DestroyTexture(outTex);
        outTex = {};
        return false;
    }

    VkDescriptorSetAllocateInfo dsAlloc{};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = descriptorPool_;
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts = &setLayout_;
    if (!CheckVk("vkAllocateDescriptorSets", devApi_.allocateDescriptorSets(device_, &dsAlloc, &outTex.set))) {
        DestroyTexture(outTex);
        outTex = {};
        return false;
    }

    const VkDescriptorImageInfo descImg{sampler_, outTex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = outTex.set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &descImg;
    devApi_.updateDescriptorSets(device_, 1, &write, 0, nullptr);
    return true;
}

void VulkanGraphicsBackend::DestroyTexture(const GpuTexture& tex) const {
    if (tex.set != VK_NULL_HANDLE) {
        VkDescriptorSet ds = tex.set;
        devApi_.freeDescriptorSets(device_, descriptorPool_, 1, &ds);
    }
    if (tex.view != VK_NULL_HANDLE) {
        devApi_.destroyImageView(device_, tex.view, nullptr);
    }
    if (tex.image != VK_NULL_HANDLE) {
        devApi_.destroyImage(device_, tex.image, nullptr);
    }
    if (tex.memory != VK_NULL_HANDLE) {
        devApi_.freeMemory(device_, tex.memory, nullptr);
    }
}

bool VulkanGraphicsBackend::CopyBufferToImage(
    VkBuffer srcBuf,
    VkImage dstImg,
    const VkBufferImageCopy* regions,
    uint32_t regionCount,
    bool isFresh) const
{
    auto makeBarrier = [dstImg](VkImageLayout oldL, VkImageLayout newL,
                                VkAccessFlags srcA, VkAccessFlags dstA) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = srcA;
        b.dstAccessMask = dstA;
        b.oldLayout = oldL;
        b.newLayout = newL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = dstImg;
        b.subresourceRange = kWholeColorImage;
        return b;
    };

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!CheckVk("vkBeginCommandBuffer", devApi_.beginCommandBuffer(commandBuffer_, &beginInfo))) {
        return false;
    }

    const VkImageMemoryBarrier toTransfer = makeBarrier(
        isFresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0,
        VK_ACCESS_TRANSFER_WRITE_BIT);
    devApi_.cmdPipelineBarrier(commandBuffer_,
                               VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    devApi_.cmdCopyBufferToImage(commandBuffer_, srcBuf, dstImg,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 regionCount, regions);

    const VkImageMemoryBarrier toShader = makeBarrier(
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT);
    devApi_.cmdPipelineBarrier(commandBuffer_,
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                               0, 0, nullptr, 0, nullptr, 1, &toShader);

    if (!CheckVk("vkEndCommandBuffer", devApi_.endCommandBuffer(commandBuffer_))) {
        return false;
    }

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer_;
    if (!CheckVk("vkQueueSubmit", devApi_.queueSubmit(queue_, 1, &submit, VK_NULL_HANDLE))) {
        return false;
    }
    return CheckVk("vkQueueWaitIdle", devApi_.queueWaitIdle(queue_));
}

bool VulkanGraphicsBackend::UploadTextureRects(
    ImTextureData& src,
    const GpuTexture& tex,
    const VkRect2D* rects,
    size_t rectCount,
    bool isFresh) const
{
    VkDeviceSize totalBytes = 0;
    for (size_t i = 0; i < rectCount; ++i) {
        totalBytes += static_cast<VkDeviceSize>(rects[i].extent.width) * rects[i].extent.height * 4ULL;
    }
    if (totalBytes == 0) {
        return true;
    }

    MappedBuffer staging{};
    if (!CreateBuffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, totalBytes, staging)) {
        return false;
    }

    std::vector<VkBufferImageCopy> regions;
    regions.reserve(rectCount);
    uint8_t* dst = staging.mapped;

    for (size_t i = 0; i < rectCount; ++i) {
        const VkRect2D& r = rects[i];
        VkBufferImageCopy copy{};
        copy.bufferOffset = static_cast<VkDeviceSize>(dst - staging.mapped);
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageOffset = {r.offset.x, r.offset.y, 0};
        copy.imageExtent = {r.extent.width, r.extent.height, 1};
        regions.push_back(copy);

        const size_t w = r.extent.width;
        for (uint32_t row = 0; row < r.extent.height; ++row, dst += w * 4U) {
            const auto* pixels = static_cast<const uint8_t*>(
                src.GetPixelsAt(r.offset.x, r.offset.y + static_cast<int>(row)));
            if (src.Format == ImTextureFormat_RGBA32) {
                std::memcpy(dst, pixels, w * 4U);
            } else {
                for (size_t col = 0; col < w; ++col) {
                    dst[col * 4 + 0] = 255;
                    dst[col * 4 + 1] = 255;
                    dst[col * 4 + 2] = 255;
                    dst[col * 4 + 3] = pixels[col];
                }
            }
        }
    }

    const bool ok = CopyBufferToImage(
        staging.buffer, tex.image, regions.data(), static_cast<uint32_t>(regions.size()), isFresh);
    DestroyBuffer(staging);
    return ok;
}

bool VulkanGraphicsBackend::UpdateTexture(ImTextureData& tex) {
    if (tex.Status == ImTextureStatus_WantCreate) {
        const VkRect2D whole{{0, 0}, {static_cast<uint32_t>(tex.Width), static_cast<uint32_t>(tex.Height)}};
        GpuTexture made{};
        if (!CreateTexture(whole.extent.width, whole.extent.height, made)) {
            return false;
        }
        if (!UploadTextureRects(tex, made, &whole, 1, true)) {
            DestroyTexture(made);
            return false;
        }
        const uint64_t id = nextTextureId_++;
        textures_.emplace(id, made);
        tex.SetTexID(static_cast<ImTextureID>(id));
        tex.SetStatus(ImTextureStatus_OK);
    } else if (tex.Status == ImTextureStatus_WantUpdates) {
        auto it = textures_.find(static_cast<uint64_t>(tex.GetTexID()));
        if (it != textures_.end() && tex.Updates.Size > 0) {
            std::vector<VkRect2D> rects;
            rects.reserve(static_cast<size_t>(tex.Updates.Size));
            for (int i = 0; i < tex.Updates.Size; ++i) {
                const ImTextureRect& r = tex.Updates[i];
                rects.push_back(VkRect2D{{r.x, r.y}, {r.w, r.h}});
            }
            if (!UploadTextureRects(tex, it->second, rects.data(), rects.size(), false)) {
                return false;
            }
        }
        tex.SetStatus(ImTextureStatus_OK);
    } else if (tex.Status == ImTextureStatus_WantDestroy && tex.UnusedFrames > 0) {
        auto it = textures_.find(static_cast<uint64_t>(tex.GetTexID()));
        if (it != textures_.end()) {
            DestroyTexture(it->second);
            textures_.erase(it);
        }
        tex.SetTexID(ImTextureID_Invalid);
        tex.SetStatus(ImTextureStatus_Destroyed);
    }
    return true;
}

bool VulkanGraphicsBackend::Create(ANativeWindow* window, int /*width*/, int /*height*/) {
    if (!window) {
        return false;
    }

    void* vkLib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!vkLib) {
        return false;
    }
    auto getInstProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(vkLib, "vkGetInstanceProcAddr"));
    auto createInst = getInstProc
        ? reinterpret_cast<PFN_vkCreateInstance>(getInstProc(nullptr, "vkCreateInstance"))
        : nullptr;
    if (!createInst) {
        return false;
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.apiVersion = VK_API_VERSION_1_0;

    const char* const instExts[2] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
    };
    VkInstanceCreateInfo instInfo{};
    instInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instInfo.pApplicationInfo = &appInfo;
    instInfo.enabledExtensionCount = 2;
    instInfo.ppEnabledExtensionNames = instExts;

    if (!CheckVk("vkCreateInstance", createInst(&instInfo, nullptr, &instance_))) {
        return false;
    }
    if (!instApi_.Load(getInstProc, instance_)) {
        Cleanup();
        return false;
    }

    if (!AttachSurface(window) || !SelectPhysicalDevice()) {
        Cleanup();
        return false;
    }

    const float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamily_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    const char* const devExts[1] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo devInfo{};
    devInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    devInfo.queueCreateInfoCount = 1;
    devInfo.pQueueCreateInfos = &queueInfo;
    devInfo.enabledExtensionCount = 1;
    devInfo.ppEnabledExtensionNames = devExts;

    if (!CheckVk("vkCreateDevice", instApi_.createDevice(physical_, &devInfo, nullptr, &device_)) ||
        !devApi_.Load(instApi_.getDeviceProcAddr, device_)) {
        Cleanup();
        return false;
    }

    VkPhysicalDeviceProperties props{};
    instApi_.getPhysicalDeviceProperties(physical_, &props);
    instApi_.getPhysicalDeviceMemoryProperties(physical_, &memProps_);
    devApi_.getDeviceQueue(device_, queueFamily_, 0, &queue_);
    if (props.limits.maxImageDimension2D > 0) {
        maxTextureSide_ = static_cast<size_t>(props.limits.maxImageDimension2D);
    }

    if (!PrepareDeviceObjects() || !RebuildSwapchain()) {
        Cleanup();
        return false;
    }

    return true;
}

bool VulkanGraphicsBackend::Rebind(ANativeWindow* window) {
    if (!window || device_ == VK_NULL_HANDLE || instance_ == VK_NULL_HANDLE) {
        return false;
    }
    if (!CheckVk("vkDeviceWaitIdle", devApi_.deviceWaitIdle(device_))) {
        return false;
    }

    if (window != boundWindow_) {
        DestroySwapchainTarget(target_);
        if (!AttachSurface(window)) {
            return false;
        }
        VkBool32 supportsPresent = VK_FALSE;
        if (!CheckVk("vkGetPhysicalDeviceSurfaceSupportKHR",
                     instApi_.getPhysicalDeviceSurfaceSupport(physical_, queueFamily_, surface_, &supportsPresent)) ||
            supportsPresent != VK_TRUE) {
            std::printf("[Vulkan] Device cannot present to rebound ANativeWindow\n");
            return false;
        }
    }

    return RebuildSwapchain();
}

bool VulkanGraphicsBackend::SetupImGui() {
    if (imguiInitialized_) {
        return true;
    }
    if (device_ == VK_NULL_HANDLE || pipeline_ == VK_NULL_HANDLE) {
        return false;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "Vulkan";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;

    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    platform.Renderer_TextureMaxWidth = static_cast<int>(maxTextureSide_);
    platform.Renderer_TextureMaxHeight = static_cast<int>(maxTextureSide_);

    imguiInitialized_ = true;
    return true;
}

void VulkanGraphicsBackend::NewFrame(ANativeWindow* /*window*/) {
    // Frame synchronization, texture updates, and swapchain handling run inside Render().
}

void VulkanGraphicsBackend::DrawFrame(const ImDrawData& frame) {
    static_assert(sizeof(ImDrawIdx) == 2, "Vulkan renderer assumes 16-bit ImDrawIdx");

    size_t vtxBytes = 0;
    size_t idxBytes = 0;
    for (int n = 0; n < frame.CmdListsCount; ++n) {
        const ImDrawList* list = frame.CmdLists[n];
        vtxBytes += static_cast<size_t>(list->VtxBuffer.Size) * sizeof(ImDrawVert);
        idxBytes += static_cast<size_t>(list->IdxBuffer.Size) * sizeof(ImDrawIdx);
    }

    if (!EnsureBufferCapacity(vertexBuf_, vtxBytes) || !EnsureBufferCapacity(indexBuf_, idxBytes)) {
        return;
    }

    uint8_t* vtxDst = vertexBuf_.mapped;
    uint8_t* idxDst = indexBuf_.mapped;
    for (int n = 0; n < frame.CmdListsCount; ++n) {
        const ImDrawList* list = frame.CmdLists[n];
        const size_t vp = static_cast<size_t>(list->VtxBuffer.Size) * sizeof(ImDrawVert);
        const size_t ip = static_cast<size_t>(list->IdxBuffer.Size) * sizeof(ImDrawIdx);
        if (vp > 0) {
            std::memcpy(vtxDst, list->VtxBuffer.Data, vp);
            vtxDst += vp;
        }
        if (ip > 0) {
            std::memcpy(idxDst, list->IdxBuffer.Data, ip);
            idxDst += ip;
        }
    }

    uint32_t imageIndex = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const VkResult acq = devApi_.acquireNextImage(
            device_, target_.swapchain, UINT64_MAX, imageAcquiredSem_, VK_NULL_HANDLE, &imageIndex);
        if (acq == VK_SUCCESS || acq == VK_SUBOPTIMAL_KHR) {
            break;
        }
        if (acq == VK_ERROR_OUT_OF_DATE_KHR && RebuildSwapchain()) {
            continue;
        }
        CheckVk("vkAcquireNextImageKHR", acq);
        return;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!CheckVk("vkBeginCommandBuffer", devApi_.beginCommandBuffer(commandBuffer_, &beginInfo))) {
        return;
    }

    const VkExtent2D extent = target_.extent;
    VkClearValue clear{};
    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = renderPass_;
    rpBegin.framebuffer = target_.framebuffers[imageIndex];
    rpBegin.renderArea = {{0, 0}, extent};
    rpBegin.clearValueCount = 1;
    rpBegin.pClearValues = &clear;

    devApi_.cmdBeginRenderPass(commandBuffer_, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
    devApi_.cmdBindPipeline(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    const VkViewport viewport{0.0f, 0.0f,
                              static_cast<float>(extent.width),
                              static_cast<float>(extent.height),
                              0.0f, 1.0f};
    devApi_.cmdSetViewport(commandBuffer_, 0, 1, &viewport);

    const ImVec2 origin = frame.DisplayPos;
    const ImVec2 fbScale = frame.FramebufferScale;
    const float sx = 2.0f * fbScale.x / viewport.width;
    const float sy = 2.0f * fbScale.y / viewport.height;
    const float pushConstants[4] = {sx, sy, -1.0f - origin.x * sx, -1.0f - origin.y * sy};
    devApi_.cmdPushConstants(commandBuffer_, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT,
                             0, sizeof(pushConstants), pushConstants);

    const VkDeviceSize zeroOffset = 0;
    devApi_.cmdBindVertexBuffers(commandBuffer_, 0, 1, &vertexBuf_.buffer, &zeroOffset);
    devApi_.cmdBindIndexBuffer(commandBuffer_, indexBuf_.buffer, 0, VK_INDEX_TYPE_UINT16);

    auto clampCoord = [](float v, uint32_t limit) -> uint32_t {
        return static_cast<uint32_t>(std::clamp(v, 0.0f, static_cast<float>(limit)));
    };

    uint32_t globalVtxOffset = 0;
    uint32_t globalIdxOffset = 0;
    VkDescriptorSet boundSet = VK_NULL_HANDLE;

    for (int n = 0; n < frame.CmdListsCount; ++n) {
        const ImDrawList* list = frame.CmdLists[n];
        for (int cmdIdx = 0; cmdIdx < list->CmdBuffer.Size; ++cmdIdx) {
            const ImDrawCmd& cmd = list->CmdBuffer[cmdIdx];
            if (cmd.UserCallback != nullptr) {
                if (cmd.UserCallback != ImDrawCallback_ResetRenderState) {
                    cmd.UserCallback(list, &cmd);
                }
                continue;
            }

            const uint32_t x0 = clampCoord((cmd.ClipRect.x - origin.x) * fbScale.x, extent.width);
            const uint32_t y0 = clampCoord((cmd.ClipRect.y - origin.y) * fbScale.y, extent.height);
            const uint32_t x1 = clampCoord((cmd.ClipRect.z - origin.x) * fbScale.x, extent.width);
            const uint32_t y1 = clampCoord((cmd.ClipRect.w - origin.y) * fbScale.y, extent.height);
            if (x1 <= x0 || y1 <= y0) {
                continue;
            }

            auto texIt = textures_.find(static_cast<uint64_t>(cmd.GetTexID()));
            if (texIt == textures_.end()) {
                continue;
            }

            const VkRect2D scissor{{static_cast<int32_t>(x0), static_cast<int32_t>(y0)},
                                   {x1 - x0, y1 - y0}};
            devApi_.cmdSetScissor(commandBuffer_, 0, 1, &scissor);

            if (texIt->second.set != boundSet) {
                boundSet = texIt->second.set;
                devApi_.cmdBindDescriptorSets(commandBuffer_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                              pipelineLayout_, 0, 1, &boundSet, 0, nullptr);
            }

            devApi_.cmdDrawIndexed(commandBuffer_,
                                   cmd.ElemCount,
                                   1,
                                   globalIdxOffset + cmd.IdxOffset,
                                   static_cast<int32_t>(globalVtxOffset + cmd.VtxOffset),
                                   0);
        }
        globalVtxOffset += static_cast<uint32_t>(list->VtxBuffer.Size);
        globalIdxOffset += static_cast<uint32_t>(list->IdxBuffer.Size);
    }

    devApi_.cmdEndRenderPass(commandBuffer_);
    if (!CheckVk("vkEndCommandBuffer", devApi_.endCommandBuffer(commandBuffer_))) {
        return;
    }

    if (!CheckVk("vkResetFences", devApi_.resetFences(device_, 1, &frameIdleFence_))) {
        return;
    }

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &imageAcquiredSem_;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commandBuffer_;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderCompleteSem_;

    if (!CheckVk("vkQueueSubmit", devApi_.queueSubmit(queue_, 1, &submit, frameIdleFence_))) {
        return;
    }

    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderCompleteSem_;
    present.swapchainCount = 1;
    present.pSwapchains = &target_.swapchain;
    present.pImageIndices = &imageIndex;

    const VkResult presRes = devApi_.queuePresent(queue_, &present);
    if (presRes == VK_ERROR_OUT_OF_DATE_KHR) {
        RebuildSwapchain();
    } else if (presRes != VK_SUCCESS && presRes != VK_SUBOPTIMAL_KHR) {
        CheckVk("vkQueuePresentKHR", presRes);
    }
}

void VulkanGraphicsBackend::Render(ImDrawData* drawData) {
    if (!drawData || device_ == VK_NULL_HANDLE || target_.swapchain == VK_NULL_HANDLE) {
        return;
    }

    // Wait for prior GPU submission before modifying textures or vertex/index buffers.
    if (!CheckVk("vkWaitForFences",
                 devApi_.waitForFences(device_, 1, &frameIdleFence_, VK_TRUE, UINT64_MAX))) {
        return;
    }

    if (drawData->Textures != nullptr) {
        for (int i = 0; i < drawData->Textures->Size; ++i) {
            ImTextureData* tex = (*drawData->Textures)[i];
            if (tex && tex->Status != ImTextureStatus_OK) {
                if (!UpdateTexture(*tex)) {
                    return;
                }
            }
        }
    }

    if (drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f) {
        return;
    }

    DrawFrame(*drawData);
}

void VulkanGraphicsBackend::ShutdownImGui() {
    if (!imguiInitialized_) {
        return;
    }

    if (device_ != VK_NULL_HANDLE && devApi_.deviceWaitIdle) {
        devApi_.deviceWaitIdle(device_);
    }

    if (ImGui::GetCurrentContext() != nullptr) {
        ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
        for (int i = 0; i < platform.Textures.Size; ++i) {
            ImTextureData* tex = platform.Textures[i];
            if (!tex || tex->GetTexID() == ImTextureID_Invalid) {
                continue;
            }
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
        ImGuiIO& io = ImGui::GetIO();
        io.BackendRendererName = nullptr;
        io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset);
        platform.Renderer_TextureMaxWidth = 0;
        platform.Renderer_TextureMaxHeight = 0;
    }

    for (const auto& entry : textures_) {
        DestroyTexture(entry.second);
    }
    textures_.clear();
    imguiInitialized_ = false;
}

void VulkanGraphicsBackend::Cleanup() {
    ShutdownImGui();

    if (device_ != VK_NULL_HANDLE) {
        if (devApi_.deviceWaitIdle) {
            devApi_.deviceWaitIdle(device_);
        }
        for (const auto& entry : textures_) {
            DestroyTexture(entry.second);
        }
        textures_.clear();

        DestroyBuffer(vertexBuf_);
        DestroyBuffer(indexBuf_);
        DestroySwapchainTarget(target_);

        if (pipeline_ != VK_NULL_HANDLE) {
            devApi_.destroyPipeline(device_, pipeline_, nullptr);
            pipeline_ = VK_NULL_HANDLE;
        }
        if (renderPass_ != VK_NULL_HANDLE) {
            devApi_.destroyRenderPass(device_, renderPass_, nullptr);
            renderPass_ = VK_NULL_HANDLE;
        }
        if (sampler_ != VK_NULL_HANDLE) {
            devApi_.destroySampler(device_, sampler_, nullptr);
            sampler_ = VK_NULL_HANDLE;
        }
        if (descriptorPool_ != VK_NULL_HANDLE) {
            devApi_.destroyDescriptorPool(device_, descriptorPool_, nullptr);
            descriptorPool_ = VK_NULL_HANDLE;
        }
        if (pipelineLayout_ != VK_NULL_HANDLE) {
            devApi_.destroyPipelineLayout(device_, pipelineLayout_, nullptr);
            pipelineLayout_ = VK_NULL_HANDLE;
        }
        if (setLayout_ != VK_NULL_HANDLE) {
            devApi_.destroyDescriptorSetLayout(device_, setLayout_, nullptr);
            setLayout_ = VK_NULL_HANDLE;
        }
        if (frameIdleFence_ != VK_NULL_HANDLE) {
            devApi_.destroyFence(device_, frameIdleFence_, nullptr);
            frameIdleFence_ = VK_NULL_HANDLE;
        }
        if (renderCompleteSem_ != VK_NULL_HANDLE) {
            devApi_.destroySemaphore(device_, renderCompleteSem_, nullptr);
            renderCompleteSem_ = VK_NULL_HANDLE;
        }
        if (imageAcquiredSem_ != VK_NULL_HANDLE) {
            devApi_.destroySemaphore(device_, imageAcquiredSem_, nullptr);
            imageAcquiredSem_ = VK_NULL_HANDLE;
        }
        if (commandPool_ != VK_NULL_HANDLE) {
            devApi_.destroyCommandPool(device_, commandPool_, nullptr);
            commandPool_ = VK_NULL_HANDLE;
            commandBuffer_ = VK_NULL_HANDLE;
        }

        devApi_.destroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
        queue_ = VK_NULL_HANDLE;
    }

    if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE && instApi_.destroySurface) {
        instApi_.destroySurface(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    boundWindow_ = nullptr;

    if (instance_ != VK_NULL_HANDLE && instApi_.destroyInstance) {
        instApi_.destroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
    physical_ = VK_NULL_HANDLE;
    queueFamily_ = UINT32_MAX;
}
