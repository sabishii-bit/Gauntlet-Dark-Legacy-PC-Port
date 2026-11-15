#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/platform/DisplayMode.h"
#include "engine/render/RenderTypes.h"

#include "FakeGameServices.h"
#include "game/config/GameConfig.h"
#include "game/menu/LiveSettings.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(std::is_abstract_v<LiveSettings>);
static_assert(std::has_virtual_destructor_v<LiveSettings>);

/** Settings answering only what all settings must, leaving the HUD looks to LiveSettings. */
class PlainSettings final : public LiveSettings {
public:
    bool save(const GameConfig& /*next*/) override { return true; }
    bool previewVideo(const GameConfig& /*trial*/) override { return false; }
    void previewAudio(const AudioConfig& /*audio*/) override {}
    DisplayOptions display() const override { return {}; }
    GraphicsApi renderer() const override { return GraphicsApi::Vulkan; }
};

TEST_CASE("fake settings count each save written back and refuse saves while failing",
          "[game][settings][live-settings]") {
    test::FakeSettings fake;
    LiveSettings& settings = fake;
    GameConfig next;
    next.audio.stereo = false;
    CHECK(settings.save(next)); // counted, though written back nowhere
    CHECK(fake.writes == 1);
    GameConfig written;
    fake.target = &written;
    std::vector<s32> seenBefore;
    fake.hook = [&](const GameConfig& config) {
        CHECK_FALSE(config.audio.stereo);
        seenBefore.push_back(fake.writes);
    };
    CHECK(settings.save(next));
    CHECK_FALSE(written.audio.stereo);
    CHECK(fake.writes == 2);
    fake.fail = true;
    next.audio.musicVolume = 0;
    CHECK_FALSE(settings.save(next));
    CHECK(written.audio.musicVolume == AudioConfig{}.musicVolume);
    CHECK(fake.writes == 2);
    CHECK(seenBefore == std::vector<s32>{1, 2}); // the hook sees the refused save too
}

TEST_CASE("fake settings keep each audio preview and answer trials, display and renderer as set",
          "[game][settings][live-settings]") {
    test::FakeSettings fake;
    LiveSettings& settings = fake;
    GameConfig trial;
    CHECK_FALSE(settings.previewVideo(trial)); // refused without an answer
    std::vector<bool> tried;
    fake.video = [&](const GameConfig& config) {
        tried.push_back(config.display.vsync);
        return config.display.vsync;
    };
    trial.display.vsync = true;
    CHECK(settings.previewVideo(trial));
    trial.display.vsync = false;
    CHECK_FALSE(settings.previewVideo(trial));
    CHECK(tried == std::vector<bool>{true, false});
    AudioConfig audio;
    audio.musicVolume = 0.25f;
    settings.previewAudio(audio);
    audio.stereo = false;
    settings.previewAudio(audio);
    REQUIRE(fake.audioPreviews.size() == 2);
    CHECK(fake.audioPreviews[0].musicVolume == 0.25f);
    CHECK(fake.audioPreviews[0].stereo);
    CHECK_FALSE(fake.audioPreviews[1].stereo);
    CHECK(fake.writes == 0); // previews never save
    CHECK(settings.display().monitors.empty());
    fake.displayOptions = {{1920, 1080}, {{1920, 1080}}, {1280, 896}, {"Left", "Right"}, 1};
    CHECK(settings.display().monitors.size() == 2);
    CHECK(settings.display().window == Extent2D{1280, 896});
    CHECK(settings.renderer() == GraphicsApi::Vulkan);
    fake.drawingApi = GraphicsApi::OpenGl;
    CHECK(settings.renderer() == GraphicsApi::OpenGl);
}

TEST_CASE("settings offer no HUD looks by default and fake settings offer those set",
          "[game][settings][live-settings]") {
    PlainSettings plain;
    LiveSettings& base = plain;
    CHECK(base.hudStyles().empty());
    test::FakeSettings fake;
    LiveSettings& settings = fake;
    CHECK(settings.hudStyles().empty());
    fake.hudOffers = {{.id = "fake", .label = "hud.style.fake", .usable = true},
                      {.id = "other", .label = "hud.style.other", .usable = false}};
    CHECK(settings.hudStyles() == fake.hudOffers);
    CHECK(fake.writes == 0); // asking saves nothing
}
} // namespace
