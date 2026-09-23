#pragma once

#include <array>
#include <memory>
#include <span>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"
#include "engine/render/vulkan/VulkanCommon.h"

namespace gdl {

class Window;
class ImmediateBatch;
class VulkanContext;
class VulkanSwapchain;
class VulkanPipeline;
class VulkanTexture;

/** Vulkan implementation of RenderDevice with two frames in flight. */
class VulkanRenderDevice final : public RenderDevice {
public:
    VulkanRenderDevice(Window& window, const RenderDeviceDesc& desc);
    ~VulkanRenderDevice() override;

    GDL_NON_COPYABLE_NON_MOVABLE(VulkanRenderDevice);

    bool beginFrame() override;
    void endFrame() override;
    void setClearColor(const Vec4& rgba) override { m_clearColor = rgba; }
    Extent2D framebufferExtent() const override;
    std::unique_ptr<Texture> createTexture(const TextureDesc& desc,
                                           std::span<const u8> rgba8Pixels) override;
    void updateTexture(Texture& texture, std::span<const u8> rgba8Pixels) override;
    const Texture& whiteTexture() const override;
    void draw(const ImmediateBatch& batch, const Texture& texture, const Mat4& transform,
              const DrawState& state) override;
    void waitIdle() override;

private:
    static constexpr u32 kFramesInFlight = 2;
    static constexpr u32 kMaxVerticesPerFrame = 1U << 18U;

    struct FrameResources {
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VmaAllocation vertexAllocation = VK_NULL_HANDLE;
        void* vertexMapped = nullptr;
        u32 vertexCursor = 0;
        VkBuffer uploadBuffer = VK_NULL_HANDLE;
        VmaAllocation uploadAllocation = VK_NULL_HANDLE;
        void* uploadMapped = nullptr;
        VkDeviceSize uploadCapacity = 0;
        VkDeviceSize uploadCursor = 0;
    };

    static constexpr usize samplerIndex(TextureFilter filter, TextureWrap across,
                                        TextureWrap down) {
        return (filter == TextureFilter::Nearest ? 1U : 0U) +
               (across == TextureWrap::ClampToEdge ? 2U : 0U) +
               (down == TextureWrap::ClampToEdge ? 4U : 0U);
    }
    VkSampler samplerFor(const TextureDesc& desc) const {
        return m_samplers[samplerIndex(desc.filter, desc.wrap, desc.wrapDown())];
    }

    void createDescriptorResources();
    void createFrameResources();
    void createPresentSemaphores();
    void destroyPresentSemaphores();
    void destroyUploadBuffer(FrameResources& frame);
    void reserveUploadBuffer(FrameResources& frame, VkDeviceSize bytes);
    void recreateSwapchain();
    void beginRendering();

    Window& m_window;
    std::unique_ptr<VulkanContext> m_context;
    std::unique_ptr<VulkanSwapchain> m_swapchain;
    std::unique_ptr<VulkanPipeline> m_pipeline;         ///< alpha blended
    std::unique_ptr<VulkanPipeline> m_additivePipeline; ///< the same, adding onto the frame
    std::unique_ptr<VulkanPipeline> m_opaquePipeline;   ///< no framebuffer blending
    BlendMode m_boundBlend = BlendMode::Alpha;

    static constexpr u32 kTexturesPerPool = 512;

    VkDescriptorPool descriptorPoolForTexture();

    VkDescriptorSetLayout m_textureSetLayout = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> m_descriptorPools; ///< each texture keeps its own
    u32 m_poolTexturesLeft = 0;                      ///< sets left in the last pool
    std::array<VkSampler, 8> m_samplers{}; ///< by samplerIndex(filter, across, down)
    std::unique_ptr<VulkanTexture> m_whiteTexture;

    std::array<FrameResources, kFramesInFlight> m_frames{};
    std::vector<VkSemaphore> m_renderFinished;
    u32 m_frameIndex = 0;
    u32 m_imageIndex = 0;
    bool m_frameOpen = false;
    bool m_renderingStarted = false;
    bool m_vertexOverflowReported = false;

    Vec4 m_clearColor{0.0f, 0.0f, 0.0f, 1.0f};
};

} // namespace gdl
