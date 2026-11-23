#pragma once

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "Graphics/GraphicsBackend.hpp"

struct ImTextureData;

/**
 * Self-contained dynamic Vulkan 1.0 renderer for Dear ImGui 1.92 Android overlay.
 *
 * - Loads libvulkan.so dynamically via dlopen() (zero link-time -lvulkan dependency,
 *   preserving android-21 launch compatibility and automatic fallback to OpenGL ES).
 * - Implements ImGuiBackendFlags_RendererHasTextures (ImTextureData dynamic font atlas)
 *   and ImGuiBackendFlags_RendererHasVtxOffset.
 * - Uses premultiplied alpha blending + PRE_MULTIPLIED/INHERIT compositeAlpha for
 *   crisp transparent SurfaceFlinger composition.
 * - Supports live Rebind(ANativeWindow*) without destroying the ImGui context.
 */
class VulkanGraphicsBackend final : public IGraphicsBackend {
public:
    VulkanGraphicsBackend() = default;
    ~VulkanGraphicsBackend() override { Cleanup(); }

    const char* GetName() const noexcept override { return "Vulkan"; }
    GraphicsBackendType GetType() const noexcept override { return GraphicsBackendType::Vulkan; }

    bool Create(ANativeWindow* window, int width, int height) override;
    bool Rebind(ANativeWindow* window) override;
    bool SetupImGui() override;
    void NewFrame(ANativeWindow* window) override;
    void Render(ImDrawData* drawData) override;
    void ShutdownImGui() override;
    void Cleanup() override;

private:
    struct InstanceApi {
        PFN_vkDestroyInstance destroyInstance = nullptr;
        PFN_vkEnumeratePhysicalDevices enumeratePhysicalDevices = nullptr;
        PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties = nullptr;
        PFN_vkGetPhysicalDeviceQueueFamilyProperties getPhysicalDeviceQueueFamilyProperties = nullptr;
        PFN_vkGetPhysicalDeviceMemoryProperties getPhysicalDeviceMemoryProperties = nullptr;
        PFN_vkCreateDevice createDevice = nullptr;
        PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;

        PFN_vkCreateAndroidSurfaceKHR createAndroidSurface = nullptr;
        PFN_vkDestroySurfaceKHR destroySurface = nullptr;
        PFN_vkGetPhysicalDeviceSurfaceSupportKHR getPhysicalDeviceSurfaceSupport = nullptr;
        PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR getPhysicalDeviceSurfaceCapabilities = nullptr;
        PFN_vkGetPhysicalDeviceSurfaceFormatsKHR getPhysicalDeviceSurfaceFormats = nullptr;

        bool Load(PFN_vkGetInstanceProcAddr getProc, VkInstance inst);
    };

    struct DeviceApi {
        PFN_vkDestroyDevice destroyDevice = nullptr;
        PFN_vkGetDeviceQueue getDeviceQueue = nullptr;
        PFN_vkDeviceWaitIdle deviceWaitIdle = nullptr;
        PFN_vkQueueSubmit queueSubmit = nullptr;
        PFN_vkQueueWaitIdle queueWaitIdle = nullptr;

        PFN_vkCreateCommandPool createCommandPool = nullptr;
        PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
        PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
        PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
        PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
        PFN_vkCreateSemaphore createSemaphore = nullptr;
        PFN_vkDestroySemaphore destroySemaphore = nullptr;
        PFN_vkCreateFence createFence = nullptr;
        PFN_vkDestroyFence destroyFence = nullptr;
        PFN_vkResetFences resetFences = nullptr;
        PFN_vkWaitForFences waitForFences = nullptr;

        PFN_vkCreateDescriptorSetLayout createDescriptorSetLayout = nullptr;
        PFN_vkDestroyDescriptorSetLayout destroyDescriptorSetLayout = nullptr;
        PFN_vkCreatePipelineLayout createPipelineLayout = nullptr;
        PFN_vkDestroyPipelineLayout destroyPipelineLayout = nullptr;
        PFN_vkCreateDescriptorPool createDescriptorPool = nullptr;
        PFN_vkDestroyDescriptorPool destroyDescriptorPool = nullptr;
        PFN_vkAllocateDescriptorSets allocateDescriptorSets = nullptr;
        PFN_vkFreeDescriptorSets freeDescriptorSets = nullptr;
        PFN_vkUpdateDescriptorSets updateDescriptorSets = nullptr;
        PFN_vkCreateRenderPass createRenderPass = nullptr;
        PFN_vkDestroyRenderPass destroyRenderPass = nullptr;
        PFN_vkCreateShaderModule createShaderModule = nullptr;
        PFN_vkDestroyShaderModule destroyShaderModule = nullptr;
        PFN_vkCreateGraphicsPipelines createGraphicsPipelines = nullptr;
        PFN_vkDestroyPipeline destroyPipeline = nullptr;
        PFN_vkCreateFramebuffer createFramebuffer = nullptr;
        PFN_vkDestroyFramebuffer destroyFramebuffer = nullptr;

        PFN_vkAllocateMemory allocateMemory = nullptr;
        PFN_vkFreeMemory freeMemory = nullptr;
        PFN_vkMapMemory mapMemory = nullptr;
        PFN_vkCreateBuffer createBuffer = nullptr;
        PFN_vkDestroyBuffer destroyBuffer = nullptr;
        PFN_vkGetBufferMemoryRequirements getBufferMemoryRequirements = nullptr;
        PFN_vkBindBufferMemory bindBufferMemory = nullptr;
        PFN_vkCreateImage createImage = nullptr;
        PFN_vkDestroyImage destroyImage = nullptr;
        PFN_vkGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
        PFN_vkBindImageMemory bindImageMemory = nullptr;
        PFN_vkCreateImageView createImageView = nullptr;
        PFN_vkDestroyImageView destroyImageView = nullptr;
        PFN_vkCreateSampler createSampler = nullptr;
        PFN_vkDestroySampler destroySampler = nullptr;

        PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
        PFN_vkCmdCopyBufferToImage cmdCopyBufferToImage = nullptr;
        PFN_vkCmdBeginRenderPass cmdBeginRenderPass = nullptr;
        PFN_vkCmdEndRenderPass cmdEndRenderPass = nullptr;
        PFN_vkCmdBindPipeline cmdBindPipeline = nullptr;
        PFN_vkCmdSetViewport cmdSetViewport = nullptr;
        PFN_vkCmdSetScissor cmdSetScissor = nullptr;
        PFN_vkCmdPushConstants cmdPushConstants = nullptr;
        PFN_vkCmdBindVertexBuffers cmdBindVertexBuffers = nullptr;
        PFN_vkCmdBindIndexBuffer cmdBindIndexBuffer = nullptr;
        PFN_vkCmdBindDescriptorSets cmdBindDescriptorSets = nullptr;
        PFN_vkCmdDrawIndexed cmdDrawIndexed = nullptr;

        PFN_vkCreateSwapchainKHR createSwapchain = nullptr;
        PFN_vkDestroySwapchainKHR destroySwapchain = nullptr;
        PFN_vkGetSwapchainImagesKHR getSwapchainImages = nullptr;
        PFN_vkAcquireNextImageKHR acquireNextImage = nullptr;
        PFN_vkQueuePresentKHR queuePresent = nullptr;

        bool Load(PFN_vkGetDeviceProcAddr getProc, VkDevice dev);
    };

    struct SwapchainTarget {
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkExtent2D extent{0, 0};
        std::vector<VkImageView> views;
        std::vector<VkFramebuffer> framebuffers;
    };

    struct MappedBuffer {
        VkBufferUsageFlags usage = 0;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize capacity = 0;
        uint8_t* mapped = nullptr;
    };

    struct GpuTexture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };

    bool AttachSurface(ANativeWindow* window);
    bool SelectPhysicalDevice();
    bool PrepareDeviceObjects();
    bool CreatePipeline();
    bool RebuildSwapchain();
    void DestroySwapchainTarget(SwapchainTarget& target) const;

    VkImageView CreateImageView(VkImage image, VkFormat format) const;
    VkDeviceMemory AllocateMemory(const VkMemoryRequirements& reqs, VkMemoryPropertyFlags flags) const;
    bool CreateBuffer(VkBufferUsageFlags usage, VkDeviceSize minSize, MappedBuffer& outBuf) const;
    void DestroyBuffer(MappedBuffer& buf) const;
    bool EnsureBufferCapacity(MappedBuffer& buf, VkDeviceSize needed) const;

    bool CreateTexture(uint32_t width, uint32_t height, GpuTexture& outTex) const;
    void DestroyTexture(const GpuTexture& tex) const;
    bool UpdateTexture(ImTextureData& tex);
    bool UploadTextureRects(
        ImTextureData& src,
        const GpuTexture& tex,
        const VkRect2D* rects,
        size_t rectCount,
        bool isFresh) const;
    bool CopyBufferToImage(
        VkBuffer srcBuf,
        VkImage dstImg,
        const VkBufferImageCopy* regions,
        uint32_t regionCount,
        bool isFresh) const;
    void DrawFrame(const ImDrawData& frame);

    InstanceApi instApi_{};
    DeviceApi devApi_{};

    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    ANativeWindow* boundWindow_ = nullptr;

    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps_{};
    uint32_t queueFamily_ = UINT32_MAX;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    size_t maxTextureSide_ = 4096;

    VkSurfaceFormatKHR surfaceFormat_{};
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
    VkSemaphore imageAcquiredSem_ = VK_NULL_HANDLE;
    VkSemaphore renderCompleteSem_ = VK_NULL_HANDLE;
    VkFence frameIdleFence_ = VK_NULL_HANDLE;

    SwapchainTarget target_{};
    MappedBuffer vertexBuf_{VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_NULL_HANDLE, VK_NULL_HANDLE, 0, nullptr};
    MappedBuffer indexBuf_{VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_NULL_HANDLE, VK_NULL_HANDLE, 0, nullptr};

    std::unordered_map<uint64_t, GpuTexture> textures_;
    uint64_t nextTextureId_ = 1;
    bool imguiInitialized_ = false;
};
