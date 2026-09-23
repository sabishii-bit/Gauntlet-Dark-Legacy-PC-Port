#include <array>
#include <cstdint>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "engine/app/Application.h"
#include "engine/math/Math.h"
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

    int renderedFrames() const { return m_renderedFrames; }
    bool sawInput() const { return m_sawInput; }

protected:
    void onInit() override {
        constexpr std::array<std::uint8_t, 16> kPixels{255, 0, 0,   255, 0,   255, 0,   255,
                                                       0,   0, 255, 255, 255, 255, 255, 255};
        m_texture = renderDevice().createTexture(TextureDesc{2, 2}, kPixels);
        REQUIRE(m_texture->width() == 2);
        REQUIRE(m_texture->height() == 2);
        m_streamed = renderDevice().createTexture(TextureDesc{8, 8}, m_image.pixels);
    }

    void onUpdate(double deltaSeconds) override {
        REQUIRE(deltaSeconds >= 0.0);
        m_sawInput = !input().isKeyDown(Key::Unknown);
    }

    void onRender(RenderDevice& device) override {
        const Extent2D extent = device.framebufferExtent();
        REQUIRE_FALSE(extent.isZero());
        const Mat4 projection = makeScreenProjection(static_cast<float>(extent.width),
                                                     static_cast<float>(extent.height));

        const auto shade = static_cast<std::uint8_t>(m_renderedFrames * 40);
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
        ice.blend = BlendMode::Opaque;
        ice.maskedTexture = m_streamed.get();
        device.draw(m_batch, *m_texture, projection, ice);
        ice.blend = BlendMode::Alpha;
        device.draw(m_batch, *m_texture, projection, ice);
        ++m_renderedFrames;
    }

    void onShutdown() override {
        m_texture.reset();
        m_streamed.reset();
    }

private:
    std::unique_ptr<Texture> m_texture;
    std::unique_ptr<Texture> m_streamed;
    Image m_image = Image::filled(8, 8, Color::black());
    ImmediateBatch m_batch;
    int m_renderedFrames = 0;
    bool m_sawInput = false;
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

} // namespace
