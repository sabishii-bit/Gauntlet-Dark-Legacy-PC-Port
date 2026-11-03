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
} // namespace
