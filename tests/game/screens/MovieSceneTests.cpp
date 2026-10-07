#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "engine/app/Application.h"
#include "engine/audio/AudioMixer.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"

#include "FakeRenderDevice.h"
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

TEST_CASE("movie volume controls decoded audio including mute changes and reopening",
          "[game][movie-volume][assets]") {
    const auto file = test::assetOrSkip("VQMOVIES/midway.avi");
    test::FakeRenderDevice device;
    AudioMixer referenceMixer{48000};
    AudioMixer adjustedMixer{48000};
    MovieScene reference;
    MovieScene adjusted;
    reference.setVolume(0.25f);
    adjusted.setVolume(0.125f);
    REQUIRE(reference.open(device, referenceMixer, file));
    REQUIRE(adjusted.open(device, adjustedMixer, file));
    std::array<f32, 3200> full{};
    std::array<f32, 3200> quiet{};
    f32 peak = 0;
    for (s32 frame = 0; frame < 90; ++frame) {
        if (frame == 30) {
            adjusted.setVolume(0);
        } else if (frame == 60) {
            adjusted.setVolume(0.125f);
        }
        REQUIRE(reference.update(1.0 / 30));
        REQUIRE(adjusted.update(1.0 / 30));
        referenceMixer.mix(full);
        adjustedMixer.mix(quiet);
        f32 error = 0;
        const f32 ratio = frame >= 30 && frame < 60 ? 0.0f : 0.5f;
        // Let the shared five-millisecond gain ramp settle after each change.
        for (usize i = 512; i < full.size(); ++i) {
            peak = std::max(peak, std::abs(full[i]));
            error = std::max(error, std::abs(quiet[i] - full[i] * ratio));
        }
        CAPTURE(frame);
        CHECK(error < 0.00001f);
    }
    REQUIRE(peak > 0.01f);
    adjusted.close();
    adjustedMixer.mix(quiet);
    adjusted.setVolume(0);
    REQUIRE(adjusted.open(device, adjustedMixer, file));
    for (s32 frame = 0; frame < 45; ++frame) {
        REQUIRE(adjusted.update(1.0 / 30));
        adjustedMixer.mix(quiet);
        CHECK(std::ranges::all_of(quiet, [](f32 sample) { return sample == 0; }));
    }
    adjusted.close();
    reference.close();
}

} // namespace
