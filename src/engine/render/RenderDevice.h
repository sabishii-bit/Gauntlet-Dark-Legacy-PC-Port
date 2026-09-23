#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

#include "engine/core/SpecialMembers.h"
#include "engine/math/Math.h"
#include "engine/render/RenderTypes.h"

namespace gdl {

class Window;
class ImmediateBatch;
class Texture;

struct RenderDeviceDesc {
    bool vsync = true;
    bool enableValidation = false;
    std::filesystem::path shaderDirectory;
};

/** How a draw combines with what is already in the frame. */
enum class BlendMode : std::uint8_t {
    Opaque,  ///< replaces the frame; texture alpha may still cut holes through alpha testing
    Alpha,   ///< blended by alpha, writing depth
    Additive ///< added onto the frame without writing depth, for glows and flames
};

/** How one batch is drawn. */
struct DrawState {
    /** The alpha test translucent surfaces use, so their clear texels neither show nor
     * write depth, like the console's compare. */
    static constexpr float kTranslucentAlphaTest = 3.0f / 255.0f;

    BlendMode blend = BlendMode::Alpha;
    /** Sampled with the vertices' second coordinates, its alpha scales the colour; null
     * leaves the colour alone. */
    const Texture* lightmap = nullptr;
    /** Alternate colour sampled at the base UV, masked by base alpha above 2/255.
     * Mutually exclusive with a lightmap; retains the surface's original blend mode. */
    const Texture* maskedTexture = nullptr;
    Vec2 uvScale{1.0f, 1.0f};  ///< every texture coordinate is scaled by this...
    Vec2 uvOffset{0.0f, 0.0f}; ///< ...then has this added
    float alphaTest = 0.0f;    ///< texels with less alpha than this are dropped; 0 keeps all
    bool cullBack = false;     ///< triangles facing away are skipped
    bool depthWrite = true;
    float darken = 0.0f; ///< how much of its colour is taken away: 0 none, 1 all

    bool operator==(const DrawState&) const = default;
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
                                                   std::span<const std::uint8_t> rgba8Pixels) = 0;

    /** Replaces a texture's pixels. Valid after beginFrame and before the frame's first draw. */
    virtual void updateTexture(Texture& texture, std::span<const std::uint8_t> rgba8Pixels) = 0;

    /** 1x1 opaque white texture for untextured drawing. */
    virtual const Texture& whiteTexture() const = 0;

    /** Draws a batch; `transform` maps positions to clip space and `state` says how the
     * texels land. Valid between beginFrame and endFrame. */
    virtual void draw(const ImmediateBatch& batch, const Texture& texture, const Mat4& transform,
                      const DrawState& state = {}) = 0;

    virtual void waitIdle() = 0;
};

std::unique_ptr<RenderDevice> createVulkanRenderDevice(Window& window,
                                                       const RenderDeviceDesc& desc);

} // namespace gdl
