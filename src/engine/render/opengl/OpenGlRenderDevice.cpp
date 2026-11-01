#include "engine/render/opengl/OpenGlRenderDevice.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

#include "engine/core/Assert.h"
#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/platform/Paths.h"
#include "engine/platform/Window.h"
#include "engine/render/ImmediateBatch.h"
#include "engine/render/opengl/OpenGlShaderSource.h"

namespace gdl {

using namespace gl; // the API's own names, in the one unit that speaks it

namespace {

constexpr std::array<std::string_view, 2> kAnisotropyExtensions{
    "GL_EXT_texture_filter_anisotropic", "GL_ARB_texture_filter_anisotropic"};

/** The text the driver returns, or `fallback` when it returns none. */
std::string driverString(const GLubyte* text, std::string_view fallback) {
    return text != nullptr ? std::string(std::bit_cast<const char*>(text)) : std::string(fallback);
}

/** A vertex attribute's byte offset, as the API takes it. */
const void* attributeOffset(usize bytes) {
    return std::bit_cast<const void*>(static_cast<std::uintptr_t>(bytes));
}

GLenum wrapMode(TextureWrap wrap) {
    return wrap == TextureWrap::ClampToEdge ? GL_CLAMP_TO_EDGE : GL_REPEAT;
}

/** The sample counts the driver offers, as the mask presentationSamples reads. */
constexpr u32 sampleMask(u32 most) {
    u32 mask = 1;
    if (most >= 2) {
        mask |= 2;
    }
    if (most >= 4) {
        mask |= 4;
    }
    return mask;
}

} // namespace

/** RGBA8 texture object with its whole mip chain, the missing tail derived by the driver. */
class OpenGlTexture final : public Texture {
public:
    OpenGlTexture(const OpenGlFunctions& functions, const TextureDesc& desc,
                  std::span<const u8> rgba8Pixels, u32 maxSize)
        : m_gl(functions), m_desc(desc),
          m_mipLevels(desc.generateMipmaps ? textureMipCount(desc.width, desc.height)
                                           : desc.mipLevels) {
        GDL_VERIFY(desc.width > 0 && desc.height > 0, "Texture dimensions must be non-zero");
        GDL_VERIFY(desc.mipLevels > 0 && desc.mipLevels <= textureMipCount(desc.width, desc.height),
                   "Texture mip count exceeds its dimensions");
        GDL_VERIFY(desc.width <= maxSize && desc.height <= maxSize,
                   "Texture dimensions exceed device limits");
        GDL_VERIFY(rgba8Pixels.size() == textureMipBytes({desc.width, desc.height}, desc.mipLevels),
                   "Texture pixel data size does not match its mip chain");
        m_gl.genTextures(1, &m_id);
        m_gl.bindTexture(GL_TEXTURE_2D, m_id);
        m_gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL,
                           static_cast<GLint>(m_mipLevels - 1));
        // The sampler objects bound at each draw say how the texels are read.
        m_gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR));
        m_gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
        usize offset = 0;
        for (u32 level = 0; level < desc.mipLevels; ++level) {
            const Extent2D size = textureMipExtent({desc.width, desc.height}, level);
            m_gl.texImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), static_cast<GLint>(GL_RGBA8),
                            static_cast<GLsizei>(size.width), static_cast<GLsizei>(size.height), 0,
                            GL_RGBA, GL_UNSIGNED_BYTE, rgba8Pixels.subspan(offset).data());
            offset += usize{size.width} * size.height * 4;
        }
        completeTail(desc.mipLevels);
    }
    ~OpenGlTexture() override { m_gl.deleteTextures(1, &m_id); }

    GDL_NON_COPYABLE_NON_MOVABLE(OpenGlTexture);

    u32 width() const override { return m_desc.width; }
    u32 height() const override { return m_desc.height; }
    GLuint id() const { return m_id; }
    const TextureDesc& description() const { return m_desc; }
    u32 mipLevels() const { return m_mipLevels; }

    /** Replaces the base level and derives the levels under it again. */
    void update(std::span<const u8> rgba8Pixels) const {
        GDL_VERIFY(rgba8Pixels.size() == usize{m_desc.width} * m_desc.height * 4,
                   "updateTexture pixel data size does not match the texture");
        m_gl.bindTexture(GL_TEXTURE_2D, m_id);
        m_gl.texSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(m_desc.width),
                           static_cast<GLsizei>(m_desc.height), GL_RGBA, GL_UNSIGNED_BYTE,
                           rgba8Pixels.data());
        completeTail(1);
    }

private:
    /** Derives every level after the `supplied` ones from the last of them. */
    void completeTail(u32 supplied) const {
        if (supplied >= m_mipLevels) {
            return;
        }
        m_gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, static_cast<GLint>(supplied - 1));
        m_gl.generateMipmap(GL_TEXTURE_2D);
        m_gl.texParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    }

    const OpenGlFunctions& m_gl;
    TextureDesc m_desc;
    u32 m_mipLevels;
    GLuint m_id = 0;
};

OpenGlRenderDevice::OpenGlRenderDevice(Window& window, const RenderDeviceDesc& desc)
    : m_window(window), m_desc(desc) {
    if (window.api() != GraphicsApi::OpenGl) {
        throw RenderDeviceUnavailable("The window was not made with an OpenGL context");
    }
    m_desc.textureFiltering = validTextureFiltering(desc.textureFiltering);
    m_desc.sampleCount = presentationSamples(desc.sampleCount, 1U | 2U | 4U);
    m_window.makeContextCurrent(true);
    try {
        loadFunctions();
        readLimits();
        m_gl.pixelStorei(GL_UNPACK_ALIGNMENT, 1);
        createSamplers();
        createPrograms();
        createVertexStream();
        constexpr std::array<u8, 4> kWhitePixel{255, 255, 255, 255};
        m_whiteTexture = std::make_unique<OpenGlTexture>(
            m_gl, TextureDesc{1, 1, TextureFilter::Nearest}, kWhitePixel, m_maxTextureSize);
        const Extent2D size = window.framebufferSize();
        if (!size.isZero() && !createTargets(size)) {
            throw RenderDeviceUnavailable("No framebuffer could be made to draw the frame into");
        }
    } catch (const RenderDeviceUnavailable&) {
        m_whiteTexture.reset();
        releaseResources();
        m_window.makeContextCurrent(false);
        throw;
    }
    reportErrors("setup");
    log::info("OpenGL render device ready: {}", m_rendererName);
}

OpenGlRenderDevice::~OpenGlRenderDevice() {
    m_gl.finish();
    m_whiteTexture.reset();
    releaseResources();
    m_window.makeContextCurrent(false);
}

void OpenGlRenderDevice::loadFunctions() {
    const std::string missing =
        m_gl.load([this](const char* name) { return m_window.openGlProcedure(name); });
    if (!missing.empty()) {
        throw RenderDeviceUnavailable("The OpenGL context lacks " + missing +
                                      "; OpenGL 3.3 is needed");
    }
}

void OpenGlRenderDevice::readLimits() {
    m_rendererName = driverString(m_gl.getString(GL_RENDERER), "unknown renderer") + ", OpenGL " +
                     driverString(m_gl.getString(GL_VERSION), "of unknown version");
    GLint value = 0;
    m_gl.getIntegerv(GL_MAX_TEXTURE_SIZE, &value);
    m_maxTextureSize = static_cast<u32>(std::max(value, 1));
    value = 0;
    m_gl.getIntegerv(GL_MAX_SAMPLES, &value);
    m_maxSamples = static_cast<u32>(std::max(value, 1));
    GLint count = 0;
    m_gl.getIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; ++i) {
        const std::string name =
            driverString(m_gl.getStringi(GL_EXTENSIONS, static_cast<GLuint>(i)), "");
        const bool anisotropy = std::ranges::any_of(
            kAnisotropyExtensions, [&name](std::string_view known) { return known == name; });
        if (anisotropy) {
            GLfloat most = 1.0f;
            m_gl.getFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &most);
            m_anisotropy = true;
            m_maxAnisotropy = std::max(most, 1.0f);
        }
    }
}

void OpenGlRenderDevice::createSamplers() {
    m_gl.genSamplers(static_cast<GLsizei>(m_samplers.size()), m_samplers.data());
    for (usize mode = 0; mode < kFiltering.size(); ++mode) {
        for (const TextureFilter filter : {TextureFilter::Linear, TextureFilter::Nearest}) {
            for (const TextureWrap across : {TextureWrap::Repeat, TextureWrap::ClampToEdge}) {
                for (const TextureWrap down : {TextureWrap::Repeat, TextureWrap::ClampToEdge}) {
                    const GLuint sampler =
                        m_samplers[mode * kSamplersPerMode + samplerIndex(filter, across, down)];
                    const bool nearest = filter == TextureFilter::Nearest;
                    const GLenum magnify = nearest ? GL_NEAREST : GL_LINEAR;
                    // Base-level sampling reads no mip, like a Vulkan sampler clamped to LOD 0.
                    const GLenum minify = kFiltering[mode] == 0 ? magnify
                                          : nearest             ? GL_NEAREST_MIPMAP_NEAREST
                                                                : GL_LINEAR_MIPMAP_LINEAR;
                    m_gl.samplerParameteri(sampler, GL_TEXTURE_MAG_FILTER,
                                           static_cast<GLint>(magnify));
                    m_gl.samplerParameteri(sampler, GL_TEXTURE_MIN_FILTER,
                                           static_cast<GLint>(minify));
                    m_gl.samplerParameteri(sampler, GL_TEXTURE_WRAP_S,
                                           static_cast<GLint>(wrapMode(across)));
                    m_gl.samplerParameteri(sampler, GL_TEXTURE_WRAP_T,
                                           static_cast<GLint>(wrapMode(down)));
                    if (m_anisotropy) {
                        m_gl.samplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT,
                                               nearest
                                                   ? 1.0f
                                                   : std::clamp(static_cast<f32>(kFiltering[mode]),
                                                                1.0f, m_maxAnisotropy));
                    }
                }
            }
        }
    }
}

GLuint OpenGlRenderDevice::samplerFor(const TextureDesc& desc, bool mipmaps) const {
    const u32 filtering = mipmaps ? validTextureFiltering(m_desc.textureFiltering) : 0;
    const auto mode =
        static_cast<usize>(std::ranges::find(kFiltering, filtering) - kFiltering.begin());
    return m_samplers[mode * kSamplersPerMode +
                      samplerIndex(desc.filter, desc.wrap, desc.wrapDown())];
}

GLuint OpenGlRenderDevice::compileShader(GLenum stage, const char* name,
                                         std::span<const std::string> defines) const {
    const std::filesystem::path file = m_desc.shaderDirectory / name;
    std::string source;
    try {
        source = openGlShaderSource(file, defines);
    } catch (const std::exception& e) {
        log::error("Cannot read shader source {}: {}", paths::utf8(file), e.what());
        GDL_FATAL("Missing shader source");
    }
    const GLuint shader = m_gl.createShader(stage);
    const GLchar* text = source.c_str();
    const auto length = static_cast<GLint>(source.size());
    m_gl.shaderSource(shader, 1, &text, &length);
    m_gl.compileShader(shader);
    GLint status = GL_FALSE;
    m_gl.getShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == GL_FALSE) {
        GLint logLength = 0;
        m_gl.getShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
        std::string report(static_cast<usize>(std::max(logLength, 1)), '\0');
        m_gl.getShaderInfoLog(shader, logLength, nullptr, report.data());
        log::error("OpenGL shader {} did not compile: {}", name, report.c_str());
        m_gl.deleteShader(shader);
        throw RenderDeviceUnavailable(std::string("The driver did not compile the shader ") + name);
    }
    return shader;
}

OpenGlRenderDevice::Program OpenGlRenderDevice::linkProgram(GLuint vertex, GLuint fragment) const {
    Program program;
    program.id = m_gl.createProgram();
    m_gl.attachShader(program.id, vertex);
    m_gl.attachShader(program.id, fragment);
    m_gl.linkProgram(program.id);
    GLint status = GL_FALSE;
    m_gl.getProgramiv(program.id, GL_LINK_STATUS, &status);
    if (status == GL_FALSE) {
        GLint logLength = 0;
        m_gl.getProgramiv(program.id, GL_INFO_LOG_LENGTH, &logLength);
        std::string report(static_cast<usize>(std::max(logLength, 1)), '\0');
        m_gl.getProgramInfoLog(program.id, logLength, nullptr, report.data());
        log::error("OpenGL program did not link: {}", report.c_str());
        m_gl.deleteProgram(program.id);
        throw RenderDeviceUnavailable("The driver did not link the immediate shader program");
    }
    program.transform = m_gl.getUniformLocation(program.id, "pc.transform");
    program.params = m_gl.getUniformLocation(program.id, "pc.params");
    program.scale = m_gl.getUniformLocation(program.id, "pc.scale");
    program.effectData0 = m_gl.getUniformLocation(program.id, "pc.effectData0");
    program.effectData1 = m_gl.getUniformLocation(program.id, "pc.effectData1");
    // The base texture reads from the first unit, the second stage from the next.
    m_gl.useProgram(program.id);
    m_gl.uniform1i(m_gl.getUniformLocation(program.id, "uImage"), 0);
    m_gl.uniform1i(m_gl.getUniformLocation(program.id, "uSecondImage"), 1);
    m_gl.useProgram(0);
    return program;
}

void OpenGlRenderDevice::createPrograms() {
    const GLuint vertex = compileShader(GL_VERTEX_SHADER, "immediate.vert", {});
    const std::array<std::string, 1> blended{"GDL_ALPHA_TO_COVERAGE 0"};
    const std::array<std::string, 1> covered{"GDL_ALPHA_TO_COVERAGE 1"};
    const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, "immediate.frag", blended);
    const GLuint coverageFragment = compileShader(GL_FRAGMENT_SHADER, "immediate.frag", covered);
    m_program = linkProgram(vertex, fragment);
    m_coverageProgram = linkProgram(vertex, coverageFragment);
    m_gl.deleteShader(vertex);
    m_gl.deleteShader(fragment);
    m_gl.deleteShader(coverageFragment);
}

void OpenGlRenderDevice::createVertexStream() {
    m_gl.genVertexArrays(1, &m_vertexArray);
    m_gl.bindVertexArray(m_vertexArray);
    m_gl.genBuffers(1, &m_vertexBuffer);
    m_gl.bindBuffer(GL_ARRAY_BUFFER, m_vertexBuffer);
    m_gl.bufferData(GL_ARRAY_BUFFER,
                    static_cast<GLsizeiptr>(usize{kMaxVerticesPerFrame} * sizeof(ImmediateVertex)),
                    nullptr, GL_STREAM_DRAW);
    constexpr auto kStride = static_cast<GLsizei>(sizeof(ImmediateVertex));
    m_gl.enableVertexAttribArray(0);
    m_gl.vertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, kStride,
                             attributeOffset(offsetof(ImmediateVertex, position)));
    m_gl.enableVertexAttribArray(1);
    m_gl.vertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, kStride,
                             attributeOffset(offsetof(ImmediateVertex, color)));
    m_gl.enableVertexAttribArray(2);
    m_gl.vertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, kStride,
                             attributeOffset(offsetof(ImmediateVertex, uv)));
    m_gl.enableVertexAttribArray(3);
    m_gl.vertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, kStride,
                             attributeOffset(offsetof(ImmediateVertex, uv2)));
}

bool OpenGlRenderDevice::makeTarget(Target& target, Extent2D size, u32 samples) {
    const auto width = static_cast<GLsizei>(size.width);
    const auto height = static_cast<GLsizei>(size.height);
    const auto count = static_cast<GLsizei>(samples > 1 ? samples : 0);
    m_gl.genRenderbuffers(1, &target.color);
    m_gl.bindRenderbuffer(GL_RENDERBUFFER, target.color);
    m_gl.renderbufferStorageMultisample(GL_RENDERBUFFER, count, GL_RGBA8, width, height);
    m_gl.genRenderbuffers(1, &target.depth);
    m_gl.bindRenderbuffer(GL_RENDERBUFFER, target.depth);
    m_gl.renderbufferStorageMultisample(GL_RENDERBUFFER, count, GL_DEPTH_COMPONENT24, width,
                                        height);
    m_gl.genFramebuffers(1, &target.framebuffer);
    m_gl.bindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    m_gl.framebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                 target.color);
    m_gl.framebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                                 target.depth);
    const GLenum status = m_gl.checkFramebufferStatus(GL_FRAMEBUFFER);
    m_gl.bindFramebuffer(GL_FRAMEBUFFER, 0);
    bool failed = status != GL_FRAMEBUFFER_COMPLETE;
    for (GLenum error = m_gl.getError(); error != GL_NO_ERROR; error = m_gl.getError()) {
        failed = true; // the storage was refused, out of memory or samples
    }
    if (failed) {
        destroyTarget(target);
    }
    return !failed;
}

void OpenGlRenderDevice::destroyTarget(Target& target) {
    if (target.framebuffer != 0) {
        m_gl.deleteFramebuffers(1, &target.framebuffer);
    }
    if (target.color != 0) {
        m_gl.deleteRenderbuffers(1, &target.color);
    }
    if (target.depth != 0) {
        m_gl.deleteRenderbuffers(1, &target.depth);
    }
    target = {};
}

bool OpenGlRenderDevice::createTargets(Extent2D size) {
    destroyTarget(m_scene);
    destroyTarget(m_resolve);
    u32 samples = presentationSamples(m_desc.sampleCount, sampleMask(m_maxSamples));
    while (!makeTarget(m_scene, size, samples)) {
        if (samples == 1) {
            return false;
        }
        samples /= 2; // the driver's count was not to be had at this size
    }
    if (samples > 1 && !makeTarget(m_resolve, size, 1)) {
        destroyTarget(m_scene);
        if (!makeTarget(m_scene, size, 1)) {
            return false;
        }
        samples = 1;
    }
    m_samples = samples;
    m_extent = size;
    m_window.setSwapInterval(m_desc.vsync ? 1 : 0);
    m_presentationPending = false;
    reportErrors("framebuffer");
    return true;
}

void OpenGlRenderDevice::releaseResources() {
    destroyTarget(m_scene);
    destroyTarget(m_resolve);
    for (Program* program : {&m_program, &m_coverageProgram}) {
        if (program->id != 0) {
            m_gl.deleteProgram(program->id);
            *program = {};
        }
    }
    if (m_vertexBuffer != 0) {
        m_gl.deleteBuffers(1, &m_vertexBuffer);
        m_vertexBuffer = 0;
    }
    if (m_vertexArray != 0) {
        m_gl.deleteVertexArrays(1, &m_vertexArray);
        m_vertexArray = 0;
    }
    if (m_samplers.front() != 0) {
        m_gl.deleteSamplers(static_cast<GLsizei>(m_samplers.size()), m_samplers.data());
        m_samplers.fill(0);
    }
}

void OpenGlRenderDevice::reportErrors(const char* where) {
    if (!m_desc.enableValidation) {
        return;
    }
    for (GLenum error = m_gl.getError(); error != GL_NO_ERROR; error = m_gl.getError()) {
        log::warn("[opengl] error 0x{:04x} after {}", error, where);
    }
}

void OpenGlRenderDevice::setPresentation(bool vsync, u32 sampleCount) {
    const u32 requested = presentationSamples(sampleCount, 1U | 2U | 4U);
    if (m_desc.vsync != vsync || m_desc.sampleCount != requested) {
        m_desc.vsync = vsync;
        m_desc.sampleCount = requested;
        m_presentationPending = true;
    }
}

bool OpenGlRenderDevice::beginFrame() {
    m_heat = {};
    GDL_ASSERT(!m_frameOpen, "beginFrame called twice without endFrame");
    const Extent2D size = m_windowSize ? *m_windowSize : m_window.framebufferSize();
    if (size.isZero()) {
        return false;
    }
    if ((m_presentationPending || size != m_extent || m_scene.framebuffer == 0) &&
        !createTargets(size)) {
        return false;
    }
    m_gl.bindFramebuffer(GL_FRAMEBUFFER, m_scene.framebuffer);
    m_gl.viewport(0, 0, static_cast<GLsizei>(m_extent.width),
                  static_cast<GLsizei>(m_extent.height));
    m_gl.disable(GL_SCISSOR_TEST);
    m_gl.disable(GL_STENCIL_TEST);
    m_gl.disable(GL_DITHER);
    m_gl.disable(GL_FRAMEBUFFER_SRGB);
    m_gl.enable(GL_MULTISAMPLE);
    m_gl.enable(GL_DEPTH_TEST);
    m_gl.depthFunc(GL_GEQUAL); // reversed Z, as the Vulkan device compares
    m_gl.depthMask(GL_TRUE);
    m_gl.disable(GL_CULL_FACE);
    // The frame is drawn in Vulkan's framebuffer coordinates, where the winding reads the
    // other way round: the faces Vulkan calls counter-clockwise are OpenGL's clockwise.
    m_gl.frontFace(GL_CW);
    m_gl.disable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    m_gl.clearColor(m_clearColor.r, m_clearColor.g, m_clearColor.b, m_clearColor.a);
    m_gl.clearDepth(0.0);
    m_gl.clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    m_gl.bindVertexArray(m_vertexArray);
    m_gl.bindBuffer(GL_ARRAY_BUFFER, m_vertexBuffer);
    // A fresh store each frame, so writing never waits for the last frame's reads.
    m_gl.bufferData(GL_ARRAY_BUFFER,
                    static_cast<GLsizeiptr>(usize{kMaxVerticesPerFrame} * sizeof(ImmediateVertex)),
                    nullptr, GL_STREAM_DRAW);
    m_vertexCursor = 0;
    m_boundProgram = 0;
    m_frameOpen = true;
    m_frameHasContent = false;
    return true;
}

void OpenGlRenderDevice::draw(const ImmediateBatch& batch, const Texture& texture,
                              const Mat4& transform, const DrawState& requested) {
    const DrawState state = resolvedDrawState(requested);
    GDL_ASSERT(m_frameOpen, "draw called outside beginFrame/endFrame");
    const auto triangles = batch.triangles();
    if (triangles.empty()) {
        return;
    }
    const auto count = static_cast<u32>(triangles.size());
    if (m_vertexCursor + count > kMaxVerticesPerFrame) {
        if (!m_vertexOverflowReported) {
            log::warn("Immediate vertex buffer overflow ({} vertices per frame); draw skipped",
                      kMaxVerticesPerFrame);
            m_vertexOverflowReported = true;
        }
        return;
    }
    m_gl.bufferSubData(GL_ARRAY_BUFFER,
                       static_cast<GLintptr>(usize{m_vertexCursor} * sizeof(ImmediateVertex)),
                       static_cast<GLsizeiptr>(triangles.size_bytes()), triangles.data());

    const bool coverage = state.usesAlphaToCoverage(m_samples);
    const Program& program = coverage ? m_coverageProgram : m_program;
    if (m_boundProgram != program.id) {
        m_gl.useProgram(program.id);
        m_boundProgram = program.id;
    }

    GDL_ASSERT(state.maskedTexture == nullptr || state.lightmap == nullptr,
               "masked colour and lightmap share the second texture stage");
    const f32 textureBlend = state.effectiveTextureBlend();
    const Texture* second = m_whiteTexture.get();
    if (state.maskedTexture != nullptr) {
        second = state.maskedTexture;
    } else if (state.lightmap != nullptr) {
        second = state.lightmap;
    } else if (textureBlend > 0.0f) {
        second = state.nextTexture;
    }
    const auto& firstTexture = dynamic_cast<const OpenGlTexture&>(texture);
    const auto& secondTexture = dynamic_cast<const OpenGlTexture&>(*second);
    m_gl.activeTexture(GL_TEXTURE0);
    m_gl.bindTexture(GL_TEXTURE_2D, firstTexture.id());
    m_gl.bindSampler(
        0, samplerFor(state.samplerDescription(firstTexture.description()), state.mipmaps));
    m_gl.activeTexture(GL_TEXTURE0 + 1);
    m_gl.bindTexture(GL_TEXTURE_2D, secondTexture.id());
    m_gl.bindSampler(1, samplerFor(state.lightmap != nullptr
                                       ? secondTexture.description()
                                       : state.samplerDescription(secondTexture.description()),
                                   state.mipmaps));

    const Vec4 params{state.uvOffset.x, state.uvOffset.y, state.alphaTest, state.darken};
    const Vec4 scale{state.uvScale.x, state.uvScale.y,
                     state.maskedTexture != nullptr ? 1.0f : -textureBlend, state.colorScale};
    const Vec4 effect{m_smoothSprites && state.usesSpriteSmoothing() &&
                              firstTexture.description().filter == TextureFilter::Linear
                          ? 1.0f
                          : 0.0f,
                      state.lightmapRgb && state.lightmap != nullptr ? -1.0f
                      : m_authoredSilhouettes && state.lightmapCutout && state.lightmap != nullptr
                          ? 1.0f
                          : 0.0f,
                      std::clamp(state.whiteBlend, 0.0f, 1.0f), state.fog.reciprocalScale};
    const Vec4 fog{state.blend == BlendMode::Additive ? Vec3{0} : state.fog.color, state.fog.bias};
    m_gl.uniformMatrix4fv(program.transform, 1, GL_FALSE, glm::value_ptr(transform));
    m_gl.uniform4fv(program.params, 1, glm::value_ptr(params));
    m_gl.uniform4fv(program.scale, 1, glm::value_ptr(scale));
    m_gl.uniform4fv(program.effectData0, 1, glm::value_ptr(effect));
    m_gl.uniform4fv(program.effectData1, 1, glm::value_ptr(fog));

    const bool additive = state.blend == BlendMode::Additive;
    // Coverage already weights the resolved colour; blending again would darken edges.
    if (state.blend == BlendMode::Opaque || coverage) {
        m_gl.disable(GL_BLEND);
    } else {
        m_gl.enable(GL_BLEND);
        m_gl.blendFuncSeparate(GL_SRC_ALPHA, additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                               additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
    }
    if (coverage) {
        m_gl.enable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    } else {
        m_gl.disable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    }
    // Additive draws never write depth; the others do unless the draw says not to.
    m_gl.depthMask(state.depthWrite && !additive ? GL_TRUE : GL_FALSE);
    m_gl.depthFunc(state.depthTest ? GL_GEQUAL : GL_ALWAYS);
    if (state.cullBack) {
        m_gl.enable(GL_CULL_FACE);
        m_gl.cullFace(GL_BACK);
    } else {
        m_gl.disable(GL_CULL_FACE);
    }
    m_gl.drawArrays(GL_TRIANGLES, static_cast<GLint>(m_vertexCursor), static_cast<GLsizei>(count));
    m_vertexCursor += count;
    m_frameHasContent = true;
}

void OpenGlRenderDevice::endFrame() {
    GDL_ASSERT(m_frameOpen, "endFrame called without beginFrame");
    m_frameOpen = false;
    const auto width = static_cast<GLint>(m_extent.width);
    const auto height = static_cast<GLint>(m_extent.height);
    m_gl.bindFramebuffer(GL_READ_FRAMEBUFFER, m_scene.framebuffer);
    if (m_samples > 1) {
        // Samples resolve only between equal rectangles; the turn comes after.
        m_gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, m_resolve.framebuffer);
        m_gl.blitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT,
                             GL_NEAREST);
        m_gl.bindFramebuffer(GL_READ_FRAMEBUFFER, m_resolve.framebuffer);
    }
    // The frame was drawn with the first row at the bottom, where Vulkan's top is; shown
    // turned over, it reads as the Vulkan device shows it.
    m_gl.bindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    m_gl.blitFramebuffer(0, 0, width, height, 0, height, width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    m_gl.bindFramebuffer(GL_FRAMEBUFFER, 0);
    reportErrors("frame");
    m_window.swapBuffers();
}

std::unique_ptr<Texture> OpenGlRenderDevice::createTexture(const TextureDesc& desc,
                                                           std::span<const u8> rgba8Pixels) {
    return std::make_unique<OpenGlTexture>(m_gl, desc, rgba8Pixels, m_maxTextureSize);
}

void OpenGlRenderDevice::updateTexture(Texture& texture, std::span<const u8> rgba8Pixels) {
    GDL_VERIFY(m_frameOpen && !m_frameHasContent,
               "updateTexture must be called after beginFrame and before the frame's first draw");
    dynamic_cast<OpenGlTexture&>(texture).update(rgba8Pixels);
}

const Texture& OpenGlRenderDevice::whiteTexture() const {
    return *m_whiteTexture;
}

void OpenGlRenderDevice::waitIdle() {
    m_gl.finish();
}

void OpenGlRenderDevice::detachFromThread() {
    m_window.makeContextCurrent(false);
}

void OpenGlRenderDevice::attachToThread() {
    m_window.makeContextCurrent(true);
}

bool OpenGlRenderDevice::declinePostProcess() {
    if (!m_postProcessDeclined) {
        log::info("Scene post-processing is not offered by the OpenGL renderer");
        m_postProcessDeclined = true;
    }
    return false;
}

bool OpenGlRenderDevice::applyDepthOfField(const DepthOfField& /*settings*/) {
    return declinePostProcess();
}

bool OpenGlRenderDevice::applyBloom() {
    return declinePostProcess();
}

bool OpenGlRenderDevice::applyAmbientOcclusion(const AmbientOcclusion& /*settings*/) {
    return declinePostProcess();
}

std::unique_ptr<RenderDevice> createOpenGlRenderDevice(Window& window,
                                                       const RenderDeviceDesc& desc) {
    return std::make_unique<OpenGlRenderDevice>(window, desc);
}

} // namespace gdl
