#include <algorithm>
#include <array>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "engine/app/Application.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/AmbientOcclusion.h"
#include "engine/render/DepthOfField.h"
#include "engine/render/HeatDistortion.h"
#include "engine/render/Image.h"
#include "engine/render/ImmediateBatch.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"

namespace {

using namespace gdl;

/** Exercises the window, device, texture upload, texture update and draw path for a few frames. */
class ProbeApplication final : public Application {
public:
    using Application::Application;
    bool changePresentation = false;
    bool changeWindow = false;
    bool depthOfField = false;
    bool bloom = false;
    bool ambientOcclusion = false;
    bool mipmaps = false;

    s32 renderedFrames() const { return m_renderedFrames; }
    bool sawInput() const { return m_sawInput; }
    s32 updates() const { return m_updates; }
    f64 elapsed() const { return m_elapsed; }
    f64 longestUpdate() const { return m_longestUpdate; }

protected:
    void onInit() override {
        constexpr std::array<u8, 16> kPixels{255, 0, 0,   255, 0,   255, 0,   255,
                                             0,   0, 255, 255, 255, 255, 255, 255};
        TextureDesc textureDesc{2, 2};
        textureDesc.generateMipmaps = mipmaps;
        m_texture = renderDevice().createTexture(textureDesc, kPixels);
        REQUIRE(m_texture->width() == 2);
        REQUIRE(m_texture->height() == 2);
        TextureDesc streamDesc{8, 8};
        streamDesc.generateMipmaps = mipmaps;
        m_streamed = renderDevice().createTexture(streamDesc, m_image.pixels);
        m_desktop = window().displayOptions().desktop;
    }

    void onUpdate(f64 deltaSeconds) override {
        REQUIRE(deltaSeconds >= 0.0);
        m_sawInput = !input().isKeyDown(Key::Unknown);
        ++m_updates;
        m_elapsed += deltaSeconds;
        m_longestUpdate = std::max(m_longestUpdate, deltaSeconds);
        if (changeWindow && m_windowStep < 4 && m_renderedFrames >= (m_windowStep + 1) * 3) {
            renderDevice().waitIdle();
            if (m_windowStep == 0) {
                REQUIRE(window().setDisplayMode(WindowMode::Windowed, {480, 320}));
                CHECK(window().windowMode() == WindowMode::Windowed);
            } else if (m_windowStep == 1) {
                REQUIRE(window().setDisplayMode(WindowMode::BorderlessFullscreen, {480, 320}));
                CHECK(window().windowMode() == WindowMode::BorderlessFullscreen);
            } else if (m_windowStep == 2) {
                REQUIRE(window().setDisplayMode(WindowMode::Fullscreen, m_desktop));
                CHECK(window().windowMode() == WindowMode::Fullscreen);
            } else {
                REQUIRE(window().setDisplayMode(WindowMode::Windowed, {320, 240}));
                CHECK(window().windowMode() == WindowMode::Windowed);
            }
            CHECK(window().displayOptions().desktop == m_desktop);
            CHECK(window().cursorCaptured() == (window().windowMode() == WindowMode::Fullscreen));
            ++m_windowStep;
        }
        if (changePresentation) {
            constexpr std::array<u32, 4> kSamples{1, 2, 4, 1};
            m_requestedSamples = kSamples[static_cast<usize>(m_renderedFrames) % kSamples.size()];
            renderDevice().setPresentation(m_renderedFrames % 2 == 0, m_requestedSamples);
        }
    }

    void onRender(RenderDevice& device) override {
        const Extent2D extent = device.framebufferExtent();
        REQUIRE_FALSE(extent.isZero());
        if (changePresentation) {
            CHECK(device.presentationSampleCount() >= 1);
            CHECK(device.presentationSampleCount() <= m_requestedSamples);
        }
        const Mat4 projection =
            makeScreenProjection(static_cast<f32>(extent.width), static_cast<f32>(extent.height));

        const auto shade = static_cast<u8>(m_renderedFrames * 40);
        m_image = Image::filled(8, 8, Color::rgba(shade, 255 - shade, shade));
        device.updateTexture(*m_streamed, m_image.pixels);

        m_batch.clear();
        m_batch.rect(Rect{8.0f, 8.0f, 64.0f, 64.0f}, 0.5f, Color::white());
        device.draw(m_batch, *m_texture, projection);
        m_batch.clear();
        m_batch.rect(Rect{80.0f, 8.0f, 64.0f, 64.0f}, 0.6f, Color::rgba(0, 128, 255, 128));
        device.draw(m_batch, device.whiteTexture(), projection);
        m_batch.clear();
        m_batch.rect(Rect{152.0f, 8.0f, 64.0f, 64.0f}, 0.7f, Color::white());
        device.draw(m_batch, *m_streamed, projection);
        // Exercise both the opaque keep-alpha pass and the return to ordinary blending.
        DrawState ice;
        ice.mipmaps = mipmaps;
        ice.blend = BlendMode::Opaque;
        ice.maskedTexture = m_streamed.get();
        device.draw(m_batch, *m_texture, projection, ice);
        ice.blend = BlendMode::Alpha;
        device.draw(m_batch, *m_texture, projection, ice);
        // An authored overlay bypasses comparison, independently of depth writes.
        ice.depthTest = false;
        ice.depthWrite = false;
        device.draw(m_batch, *m_texture, projection, ice);
        device.draw(m_batch, *m_texture, projection); // restore default depth state
        DrawState glow;
        glow.mipmaps = mipmaps;
        glow.blend = BlendMode::Additive;
        glow.depthWrite = false;
        device.draw(m_batch, *m_texture, projection, glow);
        // Flipbook interpolation shares the second stage with keep-alpha/lightmaps,
        // and a subsequent ordinary draw must not retain its frame or blend weight.
        glow.nextTexture = m_streamed.get();
        for (const f32 fraction : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
            glow.textureBlend = fraction;
            device.draw(m_batch, *m_texture, projection, glow);
            glow.blend = BlendMode::Alpha;
            device.draw(m_batch, *m_texture, projection, glow);
            glow.blend = BlendMode::Additive;
        }
        glow.lightmap = m_texture.get();
        device.draw(m_batch, *m_texture, projection, glow);
        glow.lightmap = nullptr;
        glow.maskedTexture = m_texture.get();
        device.draw(m_batch, *m_texture, projection, glow);
        device.draw(m_batch, *m_texture, projection);
        if (mipmaps) {
            for (const u32 quality : {0U, 1U, 2U, 4U, 8U, 16U}) {
                device.setTextureFiltering(quality);
                device.draw(m_batch, *m_texture, projection, glow);
                device.draw(m_batch, *m_texture, projection); // UI remains base-only
            }
        }
        if (bloom) {
            device.addHeatSource(
                HeatSource{Vec2{0.5f}, Vec2{0.1f}, 0.9f, static_cast<f32>(m_elapsed)});
            CHECK(device.applyBloom());
        }
        if (ambientOcclusion) {
            AmbientOcclusion occlusion;
            occlusion.clipToView = glm::inverse(projection);
            CHECK(device.applyAmbientOcclusion(occlusion));
        }
        if (depthOfField) {
            DepthOfField blur;
            blur.clipToView = glm::inverse(projection);
            blur.focusEnd = 0.1f;
            blur.transition = 0.1f;
            CHECK(device.applyDepthOfField(blur));
        }
        if (depthOfField || bloom || ambientOcclusion) {
            // Switching back must LOAD the postprocessed scene, retain depth, and restore
            // the normal pipeline/vertex stream rather than clearing it for the HUD.
            device.draw(m_batch, *m_texture, projection);
        }
        ++m_renderedFrames;
    }

    void onShutdown() override {
        if (changeWindow) {
            CHECK(m_windowStep == 4);
            CHECK_FALSE(window().setDisplayMode(WindowMode::Fullscreen, {1, 1}));
            CHECK_FALSE(window().setDisplayMode(WindowMode::Windowed, {0, 0}));
        }
        m_texture.reset();
        m_streamed.reset();
    }

private:
    std::unique_ptr<Texture> m_texture;
    std::unique_ptr<Texture> m_streamed;
    Image m_image = Image::filled(8, 8, Color::black());
    ImmediateBatch m_batch;
    s32 m_renderedFrames = 0;
    bool m_sawInput = false;
    s32 m_updates = 0;
    f64 m_elapsed = 0;
    f64 m_longestUpdate = 0;
    u32 m_requestedSamples = 1;
    Extent2D m_desktop;
    s32 m_windowStep = 0;
};

TEST_CASE("the application brings up a window and GPU and renders frames", "[gpu][app]") {
    ApplicationDesc desc;
    desc.window.title = "gdl tests";
    desc.window.width = 320;
    desc.window.height = 240;
    desc.maxFrames = 3;
    desc.enableValidation = false;

    ProbeApplication app(std::move(desc));
    REQUIRE(app.run() == 0);
    REQUIRE(app.renderedFrames() >= 3);
    REQUIRE(app.sawInput());
}

TEST_CASE("mipmapped draws and uploads survive filtering changes and the post chain",
          "[gpu][app][mipmaps]") {
    ApplicationDesc desc;
    desc.window.title = "gdl texture filtering test";
    desc.window.width = 320;
    desc.window.height = 240;
    desc.maxFrames = 8;
    desc.maxFrameRate = 60;
    desc.enableValidation = true;
    SECTION("single sample") {
        desc.sampleCount = 1;
    }
    SECTION("multisampled") {
        desc.sampleCount = 4;
    }
    ProbeApplication app(std::move(desc));
    app.mipmaps = true;
    app.depthOfField = true;
    app.bloom = true;
    app.ambientOcclusion = true;
    REQUIRE(app.run() == 0);
    CHECK(app.renderedFrames() >= 8);
}

TEST_CASE("the application renders between fixed-rate simulation updates", "[gpu][app][graphics]") {
    ApplicationDesc desc;
    desc.window.title = "gdl fixed-update test";
    desc.window.width = 320;
    desc.window.height = 240;
    desc.maxFrames = 12;
    desc.maxFrameRate = 60;
    desc.updateRate = 30;
    ProbeApplication app(std::move(desc));
    REQUIRE(app.run() == 0);
    CHECK(app.renderedFrames() >= 12);
    CHECK(app.updates() > 0);
    CHECK(app.elapsed() >= 1.0 / 30);
    CHECK(app.longestUpdate() <= 1.0 / 30);
    CHECK(app.sawInput());
}

TEST_CASE("Video mode changes preserve the Vulkan surface and desktop mode",
          "[gpu][app][graphics]") {
    ApplicationDesc desc;
    desc.window.title = "gdl Video mode test";
    desc.enableValidation = true;
    desc.window.width = 320;
    desc.window.height = 240;
    desc.maxFrames = 18;
    desc.maxFrameRate = 60;
    ProbeApplication app(std::move(desc));
    app.changeWindow = true;
    REQUIRE(app.run() == 0);
    CHECK(app.renderedFrames() >= 18);
}

TEST_CASE("presentation changes rebuild multisampled attachments without losing textures",
          "[gpu][app][graphics]") {
    ApplicationDesc desc;
    desc.window.title = "gdl graphics settings test";
    desc.enableValidation = true;
    desc.window.width = 320;
    desc.window.height = 240;
    desc.maxFrames = 8;
    desc.maxFrameRate = 60;
    ProbeApplication app(std::move(desc));
    app.changePresentation = true;
    REQUIRE(app.run() == 0);
    CHECK(app.renderedFrames() >= 8);
}

TEST_CASE("depth of field survives MSAA and window changes before drawing the HUD",
          "[gpu][app][dof]") {
    ApplicationDesc desc;
    desc.window.title = "gdl depth-of-field test";
    desc.enableValidation = true;
    desc.window.width = 320;
    desc.window.height = 240;
    desc.maxFrames = 18;
    desc.maxFrameRate = 60;
    ProbeApplication app(std::move(desc));
    app.depthOfField = true;
    SECTION("single sample and resizing") {
        app.changeWindow = true;
    }
    SECTION("changing multisample count") {
        app.changePresentation = true;
    }
    REQUIRE(app.run() == 0);
    CHECK(app.renderedFrames() >= 18);
}

TEST_CASE("bloom survives resize and MSAA alone and combined with depth of field",
          "[gpu][app][bloom]") {
    ApplicationDesc desc;
    desc.window.title = "gdl bloom test";
    desc.enableValidation = true;
    desc.window.width = 320;
    desc.window.height = 240;
    desc.maxFrames = 18;
    desc.maxFrameRate = 60;
    ProbeApplication app(std::move(desc));
    app.bloom = true;
    SECTION("bloom only with resizing") {
        app.changeWindow = true;
    }
    SECTION("bloom only with changing AA") {
        app.changePresentation = true;
    }
    SECTION("bloom followed by DOF with changing AA") {
        app.depthOfField = true;
        app.changePresentation = true;
    }
    SECTION("all scene effects with changing AA") {
        app.depthOfField = true;
        app.ambientOcclusion = true;
        app.changePresentation = true;
    }
    SECTION("ambient occlusion alone with resizing") {
        app.bloom = false;
        app.ambientOcclusion = true;
        app.changeWindow = true;
    }
    REQUIRE(app.run() == 0);
    CHECK(app.renderedFrames() >= 18);
}

} // namespace
