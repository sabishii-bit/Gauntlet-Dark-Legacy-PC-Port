#include <filesystem>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "engine/app/Application.h"
#include "engine/audio/AudioMixer.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"

#include "TestSupport.h"
#include "game/screens/MovieScene.h"

namespace {

using namespace gdl;
using gdl::game::MovieScene;

class MovieProbe final : public Application {
public:
    MovieProbe(ApplicationDesc desc, std::filesystem::path movie)
        : Application(std::move(desc)), m_movie(std::move(movie)) {}

    s32 renderedFrames() const { return m_renderedFrames; }
    bool opened() const { return m_opened; }

protected:
    void onInit() override { m_opened = m_scene.open(renderDevice(), m_mixer, m_movie); }

    void onUpdate(f64 deltaSeconds) override {
        if (m_opened) {
            m_scene.update(deltaSeconds);
        }
    }

    void onRender(RenderDevice& device) override {
        const Extent2D extent = device.framebufferExtent();
        const Mat4 projection =
            makeScreenProjection(static_cast<f32>(extent.width), static_cast<f32>(extent.height));
        m_scene.render(device, projection, Rect{0.0f, 0.0f, 320.0f, 240.0f});
        ++m_renderedFrames;
    }

    void onShutdown() override { m_scene.close(); }

private:
    std::filesystem::path m_movie;
    AudioMixer m_mixer{48000};
    MovieScene m_scene;
    bool m_opened = false;
    s32 m_renderedFrames = 0;
};

TEST_CASE("the movie scene renders frames of a real movie", "[gpu][assets][movie]") {
    const auto file = test::assetOrSkip("VQMOVIES/midway.avi");
    ApplicationDesc desc;
    desc.window.title = "gdl movie test";
    desc.window.width = 320;
    desc.window.height = 240;
    desc.maxFrames = 6;
    MovieProbe app(std::move(desc), file);
    REQUIRE(app.run() == 0);
    REQUIRE(app.opened());
    REQUIRE(app.renderedFrames() >= 6);
}

TEST_CASE("a closed movie scene draws nothing and reports finished", "[game][movie]") {
    MovieScene scene;
    REQUIRE_FALSE(scene.isOpen());
    REQUIRE_FALSE(scene.update(0.016));
    scene.close();
}

} // namespace
