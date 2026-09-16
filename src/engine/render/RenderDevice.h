#pragma once

#include <filesystem>
#include <memory>
#include <span>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderTypes.h"

namespace gdl {

class Window;
class ImmediateBatch;

struct RenderDeviceDesc {
    bool vsync = true;
    bool enableValidation = false;
    std::filesystem::path shaderDirectory;
};

/** The GPU interface the engine draws through. */
class RenderDevice {
public:
    RenderDevice() = default;
    virtual ~RenderDevice() = default;

    GDL_NON_COPYABLE_NON_MOVABLE(RenderDevice);

    /** Starts a frame. Returns false when nothing can be drawn this frame. */
    virtual bool beginFrame() = 0;

    /** Submits and presents the frame. */
    virtual void endFrame() = 0;

    virtual void setClearColor(const Vec4& rgba) = 0;
    virtual Extent2D framebufferExtent() const = 0;

    /** Uploads RGBA8 pixels, rows top to bottom. */
    virtual std::unique_ptr<Texture> createTexture(const TextureDesc& desc,
                                                   std::span<const u8> rgba8Pixels) = 0;

    /** Replaces a texture's pixels. Valid after beginFrame and before the frame's first draw. */
    virtual void updateTexture(Texture& texture, std::span<const u8> rgba8Pixels) = 0;

    /** 1x1 opaque white texture for untextured drawing. */
    virtual const Texture& whiteTexture() const = 0;

    /** Draws a batch; `transform` maps positions to clip space. Valid between beginFrame and
     * endFrame. */
    virtual void draw(const ImmediateBatch& batch, const Texture& texture,
                      const Mat4& transform) = 0;

    virtual void waitIdle() = 0;
};

std::unique_ptr<RenderDevice> createVulkanRenderDevice(Window& window,
                                                       const RenderDeviceDesc& desc);

} // namespace gdl
