#pragma once

#include <algorithm>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderTypes.h"
#include "engine/render/SceneFog.h"

namespace gdl {

class Window;
class ImmediateBatch;
class Texture;
struct DepthOfField;
struct AmbientOcclusion;
struct HeatSource;

/** A graphics API that cannot start on this machine: no loader, driver, context or
 * device fit for it. The application then tries the other API. */
class RenderDeviceUnavailable : public std::runtime_error {
public:
    explicit RenderDeviceUnavailable(const std::string& reason) : std::runtime_error(reason) {}
};

struct RenderDeviceDesc {
    std::string applicationName; ///< the program's name, for the graphics driver
    bool vsync = true;
    bool enableValidation = false;
    std::filesystem::path shaderDirectory;
    u32 sampleCount = 1;      ///< 1 (off), 2 or 4 samples; unsupported counts fall back safely
    u32 textureFiltering = 8; ///< base-level (0), trilinear (1), or 2/4/8/16x anisotropic
};

/** How a draw combines with what is already in the frame. */
enum class BlendMode : u8 {
    Opaque,  ///< replaces the frame; texture alpha may still cut holes through alpha testing
    Alpha,   ///< blended by alpha, writing depth
    Additive ///< added onto the frame without writing depth, for glows and flames
};

/** How one batch is drawn. */
struct DrawState {
    /** The alpha test translucent surfaces use, so their clear texels neither show nor
     * write depth, like the console's compare. */
    static constexpr f32 kTranslucentAlphaTest = 3.0f / 255.0f;

    BlendMode blend = BlendMode::Alpha;
    /** Sampled with the vertices' second coordinates, its alpha scales the colour; null
     * leaves the colour alone. */
    const Texture* lightmap = nullptr;
    /** Alternate colour sampled at the base UV, masked by base alpha above 2/255.
     * Mutually exclusive with a lightmap; retains the surface's original blend mode. */
    const Texture* maskedTexture = nullptr;
    /** Next authored flipbook frame, sampled at the base UV. A mask or lightmap takes
     * precedence because all three uses share the second texture stage. */
    const Texture* nextTexture = nullptr;
    f32 textureBlend = 0.0f;   ///< fraction toward the next frame; presentation only
    Vec2 uvScale{1.0f, 1.0f};  ///< every texture coordinate is scaled by this...
    Vec2 uvOffset{0.0f, 0.0f}; ///< ...then has this added
    f32 alphaTest = 0.0f;      ///< texels with less alpha than this are dropped; 0 keeps all
    bool cullBack = false;     ///< triangles facing away are skipped
    bool depthWrite = true;
    bool depthTest = true;     ///< false accepts every depth, independently of depth writes
    bool mipmaps = false;      ///< world draws opt in; canvas text and movies retain base sampling
    bool clampTexture = false; ///< material override; does not alter the shared texture
    /** The lightmap is an authored silhouette: its clear texels cut the draw away instead
     * of darkening it. */
    bool lightmapCutout = false;
    bool lightmapRgb = false; ///< optional imported RGB lighting; native GC uses alpha intensity
    bool alphaToCoverage = false; ///< allow MSAA coverage for depth-writing cutout surfaces
    bool smoothSprite = false;    ///< opt-in magnification of soft, camera-facing pickup art
    f32 darken = 0.0f;            ///< how much of its colour is taken away: 0 none, 1 all
    f32 colorScale =
        1.0f; ///< RGB combiner scale, clamped before alpha blending; leaves alpha alone
    f32 whiteBlend = 0.0f; ///< presentation flash toward white, preserving texture coverage
    SceneFog fog;          ///< resolved when recording a draw; UI normally has zero fog

    /** Available flipbook blend after preserving skin/lightmap ownership of stage two. */
    f32 effectiveTextureBlend() const {
        if (nextTexture == nullptr || maskedTexture != nullptr || lightmap != nullptr ||
            !(textureBlend > 0.0f)) {
            return 0.0f;
        }
        return std::min(textureBlend, 1.0f);
    }

    /** Coverage replaces blending only on solid cutouts, never on overlays or soft effects. */
    bool usesAlphaToCoverage(u32 samples) const {
        return samples > 1 && alphaToCoverage && depthTest && depthWrite &&
               blend != BlendMode::Additive && (alphaTest > 0 || maskedTexture != nullptr) &&
               effectiveTextureBlend() == 0;
    }

    bool usesSpriteSmoothing() const {
        return smoothSprite && !alphaToCoverage && blend != BlendMode::Opaque &&
               maskedTexture == nullptr && lightmap == nullptr;
    }

    bool operator==(const DrawState&) const = default;

    TextureDesc samplerDescription(TextureDesc texture) const {
        if (clampTexture) {
            texture.wrap = TextureWrap::ClampToEdge;
            texture.wrapV = TextureWrap::ClampToEdge;
        }
        return texture;
    }
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
    /** World-only draw scope; callers restore the previous value before UI/post effects. */
    void setSceneFog(const SceneFog& fog) { m_sceneFog = fog.valid() ? fog : SceneFog{}; }
    const SceneFog& sceneFog() const { return m_sceneFog; }
    DrawState resolvedDrawState(DrawState state) const {
        if (m_sceneFog.reciprocalScale != 0 || m_sceneFog.bias != 0) {
            state.fog = m_sceneFog;
        }
        return state;
    }
    virtual Extent2D framebufferExtent() const = 0;
    /** Supplies a main-thread window-size snapshot for a render worker. Direct devices
     * may continue querying their window when no override has been supplied. */
    virtual void setFramebufferSize(Extent2D /*extent*/) {}

    /** Applies presentation changes at the next drawable beginFrame, including after
     * restoration from a minimized window. Safe to request while a frame is open. */
    virtual void setPresentation(bool vsync, u32 sampleCount) = 0;
    virtual u32 presentationSampleCount() const = 0;

    virtual void setTextureFiltering(u32 filtering) = 0;
    /** Enables optional magnification filtering only on draws which explicitly opt in. */
    virtual void setSmoothSprites(bool enabled) = 0;
    /** Whether a lightmap cutout cuts; off, such a draw shows whole, as the console did. */
    virtual void setAuthoredSilhouettes(bool enabled) = 0;

    /** Uploads packed RGBA8 mip levels, rows top to bottom, largest level first. */
    virtual std::unique_ptr<Texture> createTexture(const TextureDesc& desc,
                                                   std::span<const u8> rgba8Pixels) = 0;

    /** Replaces a texture's pixels. Valid after beginFrame and before the frame's first draw. */
    virtual void updateTexture(Texture& texture, std::span<const u8> rgba8Pixels) = 0;

    /** 1x1 opaque white texture for untextured drawing. */
    virtual const Texture& whiteTexture() const = 0;

    /** Draws a batch; `transform` maps positions to clip space and `state` says how the
     * texels land. Valid between beginFrame and endFrame. */
    virtual void draw(const ImmediateBatch& batch, const Texture& texture, const Mat4& transform,
                      const DrawState& state = {}) = 0;

    virtual void waitIdle() = 0;

    /** Lets go of the thread the device was made on, before another thread takes it up
     * with attachToThread; every later call belongs to that thread. A device whose API
     * is not bound to a thread does nothing. */
    virtual void detachFromThread() {}
    virtual void attachToThread() {}

    /** Blurs the scene drawn so far, leaving subsequent HUD/menu draws sharp.
     * Returns false when the device cannot sample its depth/color buffers. */
    virtual bool applyDepthOfField(const DepthOfField& settings) = 0;
    /** Adds restrained bloom to the scene drawn so far, never to subsequent HUD/menu draws.
     * Returns false when the device cannot run scene post-processing. */
    virtual bool applyBloom() = 0;
    /** Register a live thermal emitter for this frame's Bloom pass only. */
    virtual void addHeatSource(const HeatSource& source) = 0;
    /** Contact shading before translucent effects and UI. */
    virtual bool applyAmbientOcclusion(const AmbientOcclusion& settings) = 0;

private:
    SceneFog m_sceneFog;
};

/** Either throws RenderDeviceUnavailable when its API cannot start on this machine. The
 * window must have been made for the API: a Vulkan window carries no OpenGL context. */
std::unique_ptr<RenderDevice> createVulkanRenderDevice(Window& window,
                                                       const RenderDeviceDesc& desc);
std::unique_ptr<RenderDevice> createOpenGlRenderDevice(Window& window,
                                                       const RenderDeviceDesc& desc);

} // namespace gdl
