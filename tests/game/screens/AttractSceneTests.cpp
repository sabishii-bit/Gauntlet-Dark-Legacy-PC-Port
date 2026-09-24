#include <array>

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

TEST_CASE("attract scene fails cleanly when no level catalog is supplied", "[game][attract]") {
    test::FakeRenderDevice device;
    AttractScene scene;
    REQUIRE_FALSE(scene.openNext(device, {}));
    REQUIRE(scene.update(1.0, {}) == AttractOutcome::Finished);
}

TEST_CASE("attract scene cycles eligible levels and returns to title on input",
          "[game][attract][unpacked]") {
    const auto root = test::unpackedOrSkip("wdata/TOWN.json").parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    const GameConfig config;
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
    scene.render(device, Mat4{1.0f}, 640, 480);
    REQUIRE_FALSE(device.draws.empty());
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
