#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/level/LegendsDreamcastWorldMotion.h"
#include "expansions/dreamcast/scenes/LegendsInitialPose.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Dreamcast B1 displays native pads and carries RUNEB1 throughout its pedestal reveal",
          "[legends][dreamcast-motion][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels/levelb1";
    if (!std::filesystem::exists(directory / "worlds.rom")) {
        SKIP("Optional Dreamcast extraction is not installed");
    }
    const auto bytes = readFile(directory / "worlds.rom");
    auto map = formats::LegendsMap::parseDreamcast(bytes, readFile(directory / "objects.rom"));
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
    REQUIRE(scene);
    const auto* motion = scene->dreamcastWorldMotion();
    REQUIRE(motion);
    REQUIRE(motion->switches().size() == 6);
    usize plates = 0;
    for (usize i = 0; i < scene->fixtureCount(); ++i) {
        const auto figure =
            scene->fixtureObject(LegendsSceneAdapter::kFixtureObjectBase + static_cast<s32>(i));
        if (figure && figure->name == "BRIDGEPAD" && figure->visible) {
            ++plates;
        }
    }
    CHECK(plates == 2); // four authored flag 2 volumes remain invisible, but active
    const auto rune = std::ranges::find(scene->pickups(), 144U, &LegendsPickup::nativeIndex);
    REQUIRE(rune != scene->pickups().end());
    const auto native = formats::LegendsDreamcastScene::parse(bytes);
    const Vec3 authoredRune = native.itemInstances[144].position;
    const auto support = motion->supportAt(authoredRune);
    REQUIRE(support);
    REQUIRE(support->object == 231);
    REQUIRE(motion->moves(support->object));
    const Vec3 before = rune->position;
    std::vector<LegendsPortalVisitor> visitors;
    // Visible pad 138 targets the actual supporting lift 230/FLOOR#27(231).
    // Hidden pads 141..143 separately animate the nested decorative pedestal.
    visitors.push_back({.position = motion->switchPosition(0)});
    for (usize i = 3; i < 6; ++i) {
        visitors.push_back({.position = motion->switchPosition(i)});
    }
    scene->touchSwitches(visitors);
    REQUIRE(motion->switches()[0].active);
    for (usize i = 3; i < 6; ++i) {
        REQUIRE(motion->switches()[i].active);
    }
    scene->capturePresentation();
    scene->update(.5f);
    const Vec3 middle = rune->position;
    const Vec3 expected = Vec3{motion->riderTransform(support->object) * Vec4{before, 1}};
    CHECK(glm::length(middle - expected) < .002f);
    CHECK(glm::length(middle - before) > .1f);
    CHECK(glm::length(middle - before) < 30.f);
    CHECK(scene->worldObjectTransform(245) != Mat4{1});
    scene->update(2);
    const Vec3 after = rune->position;
    CHECK(glm::length(after - middle) > .1f);
    for (s32 tick = 0; tick < 60; ++tick) {
        scene->update(1.f / 60);
    }
    CHECK(glm::length(rune->position - after) < .002f);
}
} // namespace
