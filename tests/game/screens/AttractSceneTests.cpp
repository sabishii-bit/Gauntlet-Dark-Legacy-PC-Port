#include <algorithm>
#include <array>
#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/AttractScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

std::array<WorldLocator, 3> markers() {
    std::array<WorldLocator, 3> out{};
    out[0].kind = LocatorKind::CameraAttractStart;
    out[0].position = {0, 10, 0};
    out[0].rotation.y = glm::radians(179.0f);
    out[1].kind = LocatorKind::CameraAttract;
    out[1].position = {24, 10, 0};
    out[1].rotation.y = glm::radians(-179.0f);
    out[2].kind = LocatorKind::CameraAttract;
    out[2].position = {12, 10, 0};
    out[2].rotation.y = glm::radians(-179.0f);
    return out;
}

TEST_CASE("attract rail requires an entry and visits spatially adjacent markers",
          "[game][attract]") {
    AttractCamera rail;
    REQUIRE_FALSE(rail.start({}));
    const auto points = markers();
    REQUIRE_FALSE(rail.start(std::span{points}.subspan(1)));
    REQUIRE(rail.start(points));
    REQUIRE(rail.camera().position == points[0].position);
    rail.update(0.5f);
    REQUIRE(rail.camera().position.x == Approx(6.0f));
    REQUIRE(rail.camera().yaw == Approx(glm::pi<f32>()));
    rail.update(1.5f);
    REQUIRE(rail.camera().position == points[1].position);
    REQUIRE_FALSE(rail.finished());
    rail.update(0.5f);
    REQUIRE(rail.finished());
}

TEST_CASE("attract rail handles coincident markers and large steps without looping",
          "[game][attract]") {
    auto points = markers();
    points[2].position = points[0].position;
    AttractCamera rail;
    REQUIRE(rail.start(points));
    rail.update(60.0f);
    REQUIRE(rail.finished());
    REQUIRE(rail.camera().position == points[1].position);
}

TEST_CASE("attract rail presentation samples interpolate without changing the route",
          "[game][attract][presentation]") {
    AttractCamera rail;
    const auto points = markers();
    REQUIRE(rail.start(points));
    REQUIRE(rail.presentedCamera(0).position == points[0].position);
    rail.update(0.5f);
    REQUIRE(rail.presentedCamera(0).position.x == Approx(0));
    REQUIRE(rail.presentedCamera(0.5f).position.x == Approx(3));
    REQUIRE(rail.presentedCamera(0.5f).yaw == Approx(glm::radians(179.5f)));
    REQUIRE(rail.presentedCamera(1).position.x == Approx(6));
    REQUIRE(rail.presentedCamera(2).position.x == Approx(6));
    REQUIRE(rail.presentedCamera(-1).position.x == Approx(6));
    REQUIRE(rail.camera().position.x == Approx(6));
    REQUIRE_FALSE(rail.finished());

    rail.update(1.5f);
    rail.update(0.25f);
    REQUIRE(rail.presentedCamera(0).position == points[1].position);
    REQUIRE(rail.presentedCamera(0.5f).position == points[1].position);
    REQUIRE_FALSE(rail.finished());
    rail.update(0.25f);
    REQUIRE(rail.finished());

    // A new level must not blend in the previous level's camera.
    REQUIRE(rail.start(points));
    REQUIRE(rail.presentedCamera(0).position == points[0].position);
}

TEST_CASE("attract rail preserves camera cuts and stopped updates",
          "[game][attract][presentation]") {
    auto points = markers();
    points[2].position = points[0].position;
    points[2].rotation.y = 0.0f;
    AttractCamera rail;
    REQUIRE(rail.start(points));
    rail.update(1.0f / 60.0f);
    REQUIRE(rail.presentedCamera(0).position == rail.camera().position);
    REQUIRE(rail.presentedCamera(0).yaw == rail.camera().yaw);
    rail.update(1.0f / 60.0f);
    REQUIRE(rail.presentedCamera(0).position.x < rail.camera().position.x);
    rail.update(0);
    REQUIRE(rail.presentedCamera(0).position == rail.camera().position);
    REQUIRE(rail.presentedCamera(0).yaw == rail.camera().yaw);
}

TEST_CASE("attract presentation keeps the rail speed at different render rates",
          "[game][attract][presentation]") {
    constexpr f64 kTickRate = 60.0;
    for (const s32 frameRate : {30, 60, 120, 144}) {
        CAPTURE(frameRate);
        AttractCamera rail;
        REQUIRE(rail.start(markers()));
        s32 completedTicks = 0;
        for (s32 frame = 1; frame <= frameRate; ++frame) {
            const f64 time = static_cast<f64>(frame) / frameRate;
            const auto ticks = static_cast<s32>(std::floor(time * kTickRate + 1e-8));
            while (completedTicks < ticks) {
                rail.update(static_cast<f32>(1.0 / kTickRate));
                ++completedTicks;
            }
            const auto alpha = static_cast<f32>(std::max(0.0, time * kTickRate - ticks));
            const auto shown = rail.presentedCamera(alpha);
            const f64 presentedSeconds = completedTicks > 0 ? time - 1.0 / kTickRate : 0;
            REQUIRE(shown.position.x == Approx(12 * presentedSeconds).margin(0.00002));
            REQUIRE_FALSE(rail.finished());
        }
        REQUIRE(rail.camera().position.x == Approx(12).margin(0.00002));
    }
}

TEST_CASE("attract scene fails cleanly when no level catalog is supplied", "[game][attract]") {
    test::FakeRenderDevice device;
    AttractScene scene;
    REQUIRE_FALSE(scene.openNext(device, {}));
    REQUIRE(scene.update(1.0, {}) == AttractOutcome::Finished);
}

TEST_CASE("attract scene cycles eligible levels and returns to title on input",
          "[game][attract][assets]") {
    const auto root = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    GameConfig config;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    GameContext context;
    context.config = &config;
    context.strings = &strings;
    context.levels = &levels;
    context.unpackedRoot = root;
    AttractScene scene;
    REQUIRE(scene.openNext(device, context));
    REQUIRE((scene.world().level()->selectionFlags & 2U) != 0);
    const auto initial = scene.rail().camera().position;
    MenuInput start;
    start.start = true;
    REQUIRE(scene.update(0.5, start) == AttractOutcome::Running);
    REQUIRE(scene.rail().camera().position != initial);
    scene.render(device, Mat4{1.0f}, 640, 480, 0.0f);
    REQUIRE_FALSE(device.draws.empty());
    const Mat4 startClip = device.draws.front().transform;
    CHECK(device.bloomDrawOffsets.empty());
    config.display.bloom = true;
    device.draws.clear();
    scene.render(device, Mat4{1.0f}, 640, 480, 1.0f);
    REQUIRE(device.bloomDrawOffsets.size() == 1);
    CHECK(device.bloomDrawOffsets.front() > 0);
    CHECK(device.bloomDrawOffsets.front() < device.draws.size()); // Press Start stays unprocessed.
    config.display.bloom = false;
    REQUIRE_FALSE(device.draws.empty());
    REQUIRE(device.draws.front().transform != startClip);
    const auto current = scene.rail().camera().position;
    device.draws.clear();
    scene.render(device, Mat4{1.0f}, 640, 480, 0.5f);
    REQUIRE(scene.rail().camera().position == current);
    REQUIRE(scene.update(0.6, start) == AttractOutcome::Title);
    const auto first = scene.world().ref().name;
    scene.close();
    REQUIRE_FALSE(scene.isOpen());
    REQUIRE(scene.openNext(device, context));
    REQUIRE(scene.world().ref().name != first);
    REQUIRE(scene.update(60.0, {}) == AttractOutcome::Finished);
    scene.close();
}
} // namespace
