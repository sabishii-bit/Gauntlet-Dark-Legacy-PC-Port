#pragma once

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/HeatDistortion.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"
#include "engine/render/opengl/OpenGlFunctions.h"

namespace gdl {

class Window;
class ImmediateBatch;
class OpenGlTexture;

/** OpenGL 3.3 implementation of RenderDevice, for machines whose drivers offer no Vulkan.
 * The frame is drawn into a framebuffer of its own, multisampled when asked, in the same
 * coordinates the Vulkan device draws in, and turned the right way up as it is shown; the
 * shaders are the Vulkan build's, compiled by the driver. Scene post-processing (depth
 * of field, bloom, ambient occlusion) is not offered: those passes answer false. */
class OpenGlRenderDevice final : public RenderDevice {
public:
    OpenGlRenderDevice(Window& window, const RenderDeviceDesc& desc);
    ~OpenGlRenderDevice() override;

    GDL_NON_COPYABLE_NON_MOVABLE(OpenGlRenderDevice);

    bool beginFrame() override;
    void endFrame() override;
    void setClearColor(const Vec4& rgba) override { m_clearColor = rgba; }
    Extent2D framebufferExtent() const override { return m_extent; }
    void setFramebufferSize(Extent2D extent) override { m_windowSize = extent; }
    void setPresentation(bool vsync, u32 sampleCount) override;
    u32 presentationSampleCount() const override { return m_samples; }
    void setTextureFiltering(u32 filtering) override {
        m_desc.textureFiltering = validTextureFiltering(filtering);
    }
    void setSmoothSprites(bool enabled) override { m_smoothSprites = enabled; }
    void setAuthoredSilhouettes(bool enabled) override { m_authoredSilhouettes = enabled; }
    std::unique_ptr<Texture> createTexture(const TextureDesc& desc,
                                           std::span<const u8> rgba8Pixels) override;
    void updateTexture(Texture& texture, std::span<const u8> rgba8Pixels) override;
    const Texture& whiteTexture() const override;
    void draw(const ImmediateBatch& batch, const Texture& texture, const Mat4& transform,
              const DrawState& requested) override;
    void waitIdle() override;
    void detachFromThread() override;
    void attachToThread() override;
    bool applyDepthOfField(const DepthOfField& settings) override;
    bool applyBloom() override;
    void addHeatSource(const HeatSource& source) override { m_heat.add(source); }
    bool applyAmbientOcclusion(const AmbientOcclusion& settings) override;

    /** What the context reports itself as, for the log. */
    const std::string& rendererName() const { return m_rendererName; }

private:
    static constexpr u32 kMaxVerticesPerFrame = 1U << 18U;
    static constexpr std::array<u32, 6> kFiltering{0, 1, 2, 4, 8, 16};
    static constexpr usize kSamplersPerMode = 8;

    /** A linked program and where its draw constants go. */
    struct Program {
        gl::GLuint id = 0;
        gl::GLint transform = -1;
        gl::GLint params = -1;
        gl::GLint scale = -1;
        gl::GLint effectData0 = -1;
        gl::GLint effectData1 = -1;
    };
    /** A colour and depth pair drawn into, at the frame's size. */
    struct Target {
        gl::GLuint framebuffer = 0;
        gl::GLuint color = 0;
        gl::GLuint depth = 0;
    };

    static constexpr usize samplerIndex(TextureFilter filter, TextureWrap across,
                                        TextureWrap down) {
        return (filter == TextureFilter::Nearest ? 1U : 0U) +
               (across == TextureWrap::ClampToEdge ? 2U : 0U) +
               (down == TextureWrap::ClampToEdge ? 4U : 0U);
    }
    gl::GLuint samplerFor(const TextureDesc& desc, bool mipmaps) const;

    void loadFunctions();
    void readLimits();
    void createSamplers();
    void createPrograms();
    gl::GLuint compileShader(gl::GLenum stage, const char* name,
                             std::span<const std::string> defines) const;
    Program linkProgram(gl::GLuint vertex, gl::GLuint fragment) const;
    void createVertexStream();
    /** Makes the frame's targets at `size` with the samples asked for, or fewer when the
     * driver refuses them; false when nothing could be made. */
    bool createTargets(Extent2D size);
    void destroyTarget(Target& target);
    bool makeTarget(Target& target, Extent2D size, u32 samples);
    void releaseResources();
    /** Logs what the driver has flagged since the last call; only with validation on. */
    void reportErrors(const char* where);
    bool declinePostProcess();

    Window& m_window;
    std::optional<Extent2D> m_windowSize;
    RenderDeviceDesc m_desc;
    gl::OpenGlFunctions m_gl;
    std::string m_rendererName;
    bool m_smoothSprites = false;
    bool m_authoredSilhouettes = true;
    bool m_presentationPending = true;
    bool m_anisotropy = false;
    f32 m_maxAnisotropy = 1.0f;
    u32 m_maxTextureSize = 0;
    u32 m_maxSamples = 1;
    u32 m_samples = 1;
    Extent2D m_extent;
    Target m_scene;
    Target m_resolve;  ///< single-sampled, where a multisampled scene resolves before showing
    Program m_program; ///< alpha blended, opaque and additive draws
    Program m_coverageProgram; ///< MSAA coverage instead of alpha blending
    gl::GLuint m_vertexArray = 0;
    gl::GLuint m_vertexBuffer = 0;
    u32 m_vertexCursor = 0;
    std::array<gl::GLuint, kSamplersPerMode * kFiltering.size()> m_samplers{};
    std::unique_ptr<OpenGlTexture> m_whiteTexture;
    HeatDistortion m_heat;
    gl::GLuint m_boundProgram = 0;
    bool m_frameOpen = false;
    bool m_frameHasContent = false;
    bool m_postProcessDeclined = false;
    bool m_vertexOverflowReported = false;
    Vec4 m_clearColor{0.0f, 0.0f, 0.0f, 1.0f};
};

} // namespace gdl
