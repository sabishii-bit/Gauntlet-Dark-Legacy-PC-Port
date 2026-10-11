#pragma once

#include "engine/render/RenderDevice.h"

namespace gdl {
/** Records immutable draw packets on the application thread and executes Vulkan work on
 * one worker. At most one frame is recording, queued or executing: beginFrame returns
 * false instead of waiting for presentation. Gameplay never accesses worker-owned GPU
 * resources. Texture proxies and packets retain resources through their last draw.
 * All public methods belong to the application thread; waitIdle is an explicit barrier
 * for loading/shutdown, not a per-frame operation. CPU scene traversal still runs there. */
class ThreadedRenderDevice final : public RenderDevice {
public:
    explicit ThreadedRenderDevice(std::unique_ptr<RenderDevice> backend, Extent2D extent);
    ~ThreadedRenderDevice() override;
    bool beginFrame() override;
    void endFrame() override;
    void setClearColor(const Vec4& rgba) override;
    Extent2D framebufferExtent() const override;
    void setFramebufferSize(Extent2D extent) override;
    void setPresentation(bool vsync, u32 sampleCount) override;
    u32 presentationSampleCount() const override;
    void setTextureFiltering(u32 filtering) override;
    void setSmoothSprites(bool enabled) override;
    std::unique_ptr<Texture> createTexture(const TextureDesc& desc,
                                           std::span<const u8> pixels) override;
    void updateTexture(Texture& texture, std::span<const u8> pixels) override;
    const Texture& whiteTexture() const override;
    void draw(const ImmediateBatch& batch, const Texture& texture, const Mat4& transform,
              const DrawState& state = {}) override;
    void waitIdle() override;
    bool applyDepthOfField(const DepthOfField& settings) override;
    bool applyBloom() override;
    void addHeatSource(const HeatSource& source) override;
    bool applyAmbientOcclusion(const AmbientOcclusion& settings) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace gdl
