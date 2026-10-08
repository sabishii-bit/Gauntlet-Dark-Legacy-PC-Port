#pragma once

#include <array>
#include <memory>
#include <span>
#include <vector>

#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/HeatDistortion.h"
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
class VulkanPostProcess;

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
    void setPresentation(bool vsync, u32 sampleCount) override;
    u32 presentationSampleCount() const override;
    void setTextureFiltering(u32 filtering) override {
        m_desc.textureFiltering = validTextureFiltering(filtering);
    }
    void setSmoothSprites(bool enabled) override { m_smoothSprites = enabled; }
    std::unique_ptr<Texture> createTexture(const TextureDesc& desc,
                                           std::span<const u8> rgba8Pixels) override;
    void updateTexture(Texture& texture, std::span<const u8> rgba8Pixels) override;
    const Texture& whiteTexture() const override;
    void draw(const ImmediateBatch& batch, const Texture& texture, const Mat4& transform,
              const DrawState& state) override;
    void waitIdle() override;
    bool applyDepthOfField(const DepthOfField& settings) override;
    bool applyBloom() override;
    void addHeatSource(const HeatSource& source) override { m_heat.add(source); }
    bool applyAmbientOcclusion(const AmbientOcclusion& settings) override;

private:
    HeatDistortion m_heat;
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
    VkDescriptorSet samplerSetFor(const TextureDesc& desc, bool mipmaps) const;

    void createDescriptorResources();
    void createPipelines();
    void createFrameResources();
    void createPresentSemaphores();
    void destroyPresentSemaphores();
    void destroyUploadBuffer(FrameResources& frame);
    void reserveUploadBuffer(FrameResources& frame, VkDeviceSize bytes);
    void recreateSwapchain();
    void beginRendering();
    bool preparePostProcess();

    Window& m_window;
    RenderDeviceDesc m_desc;
    bool m_smoothSprites = false;
    bool m_presentationPending = false;
    std::unique_ptr<VulkanContext> m_context;
    std::unique_ptr<VulkanSwapchain> m_swapchain;
    std::unique_ptr<VulkanPipeline> m_pipeline;         ///< alpha blended
    std::unique_ptr<VulkanPipeline> m_additivePipeline; ///< the same, adding onto the frame
    std::unique_ptr<VulkanPipeline> m_opaquePipeline;   ///< no framebuffer blending
    std::unique_ptr<VulkanPipeline> m_coveragePipeline; ///< MSAA coverage instead of alpha blending
    std::unique_ptr<VulkanPostProcess> m_postProcess;
    BlendMode m_boundBlend = BlendMode::Alpha;
    bool m_boundCoverage = false;

    static constexpr u32 kTexturesPerPool = 512;

    VkDescriptorPool descriptorPoolForTexture();

    VkDescriptorSetLayout m_textureSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_postTextureSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_samplerSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_samplerPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> m_descriptorPools; ///< each texture keeps its own
    u32 m_poolTexturesLeft = 0;                      ///< sets left in the last pool
    static constexpr std::array<u32, 6> kFiltering{0, 1, 2, 4, 8, 16};
    static constexpr usize kSamplersPerMode = 8;
    std::array<VkSampler, kSamplersPerMode * kFiltering.size()> m_samplers{};
    std::array<VkDescriptorSet, kSamplersPerMode * kFiltering.size()> m_samplerSets{};
    std::unique_ptr<VulkanTexture> m_whiteTexture;

    std::array<FrameResources, kFramesInFlight> m_frames{};
    std::vector<VkSemaphore> m_renderFinished;
    u32 m_frameIndex = 0;
    u32 m_imageIndex = 0;
    bool m_frameOpen = false;
    bool m_renderingStarted = false;
    bool m_frameHasContent = false;
    bool m_postProcessUnsupportedReported = false;
    bool m_vertexOverflowReported = false;

    Vec4 m_clearColor{0.0f, 0.0f, 0.0f, 1.0f};
};

} // namespace gdl
