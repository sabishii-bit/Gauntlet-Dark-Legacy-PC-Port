#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <stdexcept>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/n64/level/LegendsPortals.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Phase = LegendsPortalMotion::Phase;

TEST_CASE("Legends departure spins and shrinks in place for the native two seconds",
          "[legends][portal]") {
    const auto start = legendsPortalDeparture(0);
    CHECK(start.yaw == 0);
    CHECK(start.scale == 1);
    CHECK_FALSE(start.finished);
    const auto middle = legendsPortalDeparture(1);
    CHECK(middle.yaw == Catch::Approx(9.3f));
    CHECK(middle.scale == Catch::Approx(.55f));
    CHECK_FALSE(middle.finished);
    const auto end = legendsPortalDeparture(kLegendsPortalDepartureSeconds);
    CHECK(end.yaw == Catch::Approx(18.6f));
    CHECK(end.scale == Catch::Approx(.1f));
    CHECK(end.finished);
    for (const f32 time : {2.01f, 10.0f, std::numeric_limits<f32>::max()}) {
        const auto held = legendsPortalDeparture(time);
        CHECK(held.yaw == end.yaw);
        CHECK(held.scale == end.scale);
        CHECK(held.finished);
    }
    for (const f32 invalid :
         {-1.0f, std::numeric_limits<f32>::infinity(), std::numeric_limits<f32>::quiet_NaN()}) {
        const auto sample = legendsPortalDeparture(invalid);
        CHECK(sample.yaw == 0);
        CHECK(sample.scale == 1);
        CHECK_FALSE(sample.finished);
    }
}

TEST_CASE("Legends portal contact uses native cylinder height and GDL player reach",
          "[legends][portal]") {
    formats::LegendsN64Portal portal;
    portal.position = {10, 2, -4};
    portal.radius = 2.5f;
    portal.height = 4;
    LegendsPortalVisitor visitor{0, {13, 2, -4}, true, .75f, 3};
    REQUIRE(touchesLegendsPortal(portal, visitor));
    visitor.position.x = 13.25f;
    CHECK_FALSE(touchesLegendsPortal(portal, visitor));
    visitor.position = {10, 6.001f, -4};
    CHECK_FALSE(touchesLegendsPortal(portal, visitor));
    visitor.position.y = -1.001f;
    CHECK_FALSE(touchesLegendsPortal(portal, visitor));
    visitor.position.y = -1;
    CHECK(touchesLegendsPortal(portal, visitor));
    visitor.alive = false;
    CHECK_FALSE(touchesLegendsPortal(portal, visitor));
    visitor.alive = true;
    visitor.player = 4;
    CHECK_FALSE(touchesLegendsPortal(portal, visitor));
    visitor.player = 0;
    visitor.position.x = std::numeric_limits<f32>::quiet_NaN();
    CHECK_FALSE(touchesLegendsPortal(portal, visitor));
}

TEST_CASE("Legends exits play startup and wait before one-shot departure", "[legends][portal]") {
    LegendsPortalMotion portal{20, true, true};
    CHECK(portal.phase() == Phase::Idle);
    REQUIRE_FALSE(portal.advance(1.0f / 30, 1, 1));
    CHECK(portal.phase() == Phase::Starting);
    CHECK(portal.waitingTicks() == 50);
    REQUIRE_FALSE(portal.advance(1.0f / 30, 1, 1));
    CHECK(portal.waitingTicks() == 59);
    portal.finishPresentation();
    CHECK(portal.phase() == Phase::Active);
    for (s32 i = 0; i < 59; ++i) {
        REQUIRE_FALSE(portal.advance(1.0f / 30, 1, 1));
    }
    REQUIRE(portal.advance(1.0f / 30, 1, 1));
    CHECK(portal.phase() == Phase::Departed);
    for (s32 i = 0; i < 90; ++i) {
        REQUIRE_FALSE(portal.advance(1.0f / 30, 1, 1));
    }
}

TEST_CASE("Revoked Legends portal access discards charge and fractional readiness",
          "[legends][portal][unlocks]") {
    LegendsPortalMotion portal{20, true, true};
    for (s32 tick = 0; tick < 120; ++tick) {
        REQUIRE_FALSE(portal.advance(1.0f / 60, 1, 1));
    }
    REQUIRE(portal.phase() == Phase::Starting);
    CHECK_FALSE(portal.advance(0, 1, 1, false));
    CHECK(portal.phase() == Phase::Idle);
    CHECK(portal.waitingTicks() == 0);
    for (s32 tick = 0; tick < 600; ++tick) {
        CHECK_FALSE(portal.advance(1.0f / 60, 1, 1, false));
    }
    CHECK(portal.phase() == Phase::Idle);
    for (s32 tick = 0; tick < 120; ++tick) {
        REQUIRE_FALSE(portal.advance(1.0f / 60, 1, 1));
    }
    bool departed = false;
    for (s32 tick = 0; tick < 10; ++tick) {
        departed |= portal.advance(1.0f / 60, 1, 1);
    }
    CHECK(departed);
}

TEST_CASE(
    "Legends exit requires every living player and restarts readiness when membership changes",
    "[legends][portal]") {
    LegendsPortalMotion portal{20, true, true};
    for (s32 i = 0; i < 120; ++i) {
        REQUIRE_FALSE(portal.advance(1.0f / 30, 1, 15));
    }
    CHECK(portal.waitingTicks() == 0);
    // First three arrive; the fourth living player still blocks the transition.
    for (s32 i = 0; i < 90; ++i) {
        REQUIRE_FALSE(portal.advance(1.0f / 30, 7, 15));
    }
    for (s32 i = 0; i < 60; ++i) {
        REQUIRE_FALSE(portal.advance(1.0f / 30, 15, 15));
    }
    REQUIRE(portal.advance(1.0f / 30, 15, 15));
    LegendsPortalMotion survivors;
    for (s32 i = 0; i < 61; ++i) {
        REQUIRE_FALSE(survivors.advance(1.0f / 30, 5, 5));
    }
    CHECK(survivors.advance(1.0f / 30, 5, 5));
}

TEST_CASE("Vacant Legends portals close and may reactivate during closing", "[legends][portal]") {
    LegendsPortalMotion portal{20, true, true};
    for (s32 i = 0; i < 40; ++i) {
        REQUIRE_FALSE(portal.advance(1.0f / 30, 1, 1));
    }
    portal.finishPresentation();
    for (s32 i = 0; i < 60; ++i) {
        REQUIRE_FALSE(portal.advance(1.0f / 30, 0, 1));
    }
    REQUIRE_FALSE(portal.advance(1.0f / 30, 0, 1));
    CHECK(portal.phase() == Phase::Closing);
    SECTION("closing finishes") {
        portal.finishPresentation();
        CHECK(portal.phase() == Phase::Idle);
    }
    SECTION("reentry cancels closing") {
        const auto generation = portal.generation();
        REQUIRE_FALSE(portal.advance(1.0f / 30, 1, 1));
        CHECK(portal.phase() == Phase::Starting);
        CHECK(portal.generation() > generation);
        CHECK(portal.waitingTicks() == 50);
    }
}

TEST_CASE("Legends portal native clock is independent of update rate and rejects invalid input",
          "[legends][portal]") {
    for (const s32 frequency : {30, 60, 144}) {
        CAPTURE(frequency);
        LegendsPortalMotion portal{20, true, true};
        s32 emitted = 0;
        f32 elapsed = 0;
        for (s32 i = 0; i < frequency * 3; ++i) {
            if (portal.advance(1.0f / static_cast<f32>(frequency), 1, 1)) {
                ++emitted;
                elapsed = static_cast<f32>(i + 1) / static_cast<f32>(frequency);
            }
        }
        CHECK(emitted == 1);
        CHECK(elapsed >= 62.0f / 30 - .0001f);
        CHECK(elapsed < 62.0f / 30 + 1.0f / static_cast<f32>(frequency) + .0001f);
    }
    LegendsPortalMotion portal;
    for (const f32 invalid : {0.0f, -1.0f, 11.0f, std::numeric_limits<f32>::infinity(),
                              std::numeric_limits<f32>::quiet_NaN()}) {
        CHECK_FALSE(portal.advance(invalid, 1, 1));
        CHECK(portal.phase() == Phase::Idle);
    }
    CHECK_FALSE(portal.advance(1, 16, 1));
    CHECK_FALSE(portal.advance(1, 1, 16));
    CHECK(portal.phase() == Phase::Idle);
    CHECK_THROWS_AS(LegendsPortalMotion(65536), std::invalid_argument);
    for (s32 i = 0; i < 100; ++i) {
        CHECK_FALSE(portal.advance(1.0f / 30, 1, 0));
    }
    CHECK(portal.phase() == Phase::Idle);
}

TEST_CASE("Native Tower exit artwork activates before party-wide travel without runtime uploads",
          "[legends][portal][tower][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/tower2";
    if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
        SKIP("N64 Legends Tower extraction is not installed");
    }
    const auto objects = readFile(directory / "objects.wad");
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), objects,
                                             readFile(directory / "ipr.rom"));
    for (auto& texture : formats::decodeLegendsN64Textures(objects)) {
        map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                texture.hasTransparency, std::move(texture.name), texture.external,
                                texture.width, texture.height});
    }
    test::FakeRenderDevice device;
    auto scene = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map, 2);
    REQUIRE(scene->portals().size() == 24);
    const CharacterSave saved;
    const std::array<const CharacterSave*, 1> party{&saved};
    scene->synchronizeProgress(party);
    const auto& portal = scene->portals()[5]; // open Mountain realm's first exit
    REQUIRE(portal.map == "mount1");
    std::array<LegendsPortalVisitor, 2> visitors{
        LegendsPortalVisitor{0, portal.position, true},
        LegendsPortalVisitor{1, portal.position + Vec3{12, 0, 0}, true}};
    const auto sample = [&] {
        device.draws.clear();
        scene->draw(device, Mat4{1}, CameraFrame::of(WorldCamera{}), TreeModel::Pass::All);
        std::vector<std::pair<const Texture*, ImmediateVertex>> vertices;
        for (const auto& draw : device.draws) {
            for (const auto& vertex : draw.vertices) {
                const Vec3 world = Vec3(draw.transform * Vec4(vertex.position, 1));
                const Vec2 offset{world.x - portal.position.x, world.z - portal.position.z};
                if (glm::dot(offset, offset) <= 16 && world.y >= portal.position.y - 1 &&
                    world.y <= portal.position.y + 15) {
                    auto transformed = vertex;
                    transformed.position = world;
                    vertices.emplace_back(draw.texture, transformed);
                }
            }
        }
        return vertices;
    };
    const auto before = sample();
    REQUIRE_FALSE(before.empty());
    const auto height = [&](const auto& vertices) {
        f32 top = portal.position.y;
        for (const auto& vertex : vertices) {
            top = std::max(top, vertex.second.position.y);
        }
        return top - portal.position.y;
    };
    CHECK(height(before) < 2);
    const auto texturesBefore = device.texturesCreated;
    // One player's arrival is enough to animate, never enough to move two.
    for (s32 tick = 0; tick < 180; ++tick) {
        scene->capturePresentation();
        scene->update(1.0f / 60);
        REQUIRE_FALSE(scene->tickPortals(visitors, 1.0f / 60));
        if (tick == 16) {
            // Native EXNS property16 raises the portal surface over15 frames.
            const auto opening = sample();
            CHECK(height(opening) > 5);
            CHECK(height(opening) < 9);
        }
    }
    const auto active = sample();
    CHECK_FALSE(active.empty());
    CHECK(active != before);
    CHECK(height(active) > 12);
    CHECK(device.texturesCreated == texturesBefore);
    visitors[1].position = portal.position;
    std::optional<std::string> destination;
    s32 departureTick = -1;
    for (s32 tick = 0; tick < 180; ++tick) {
        scene->capturePresentation();
        scene->update(1.0f / 60);
        const auto beforeDeparture = sample();
        if (const auto request = scene->tickPortals(visitors, 1.0f / 60)) {
            REQUIRE_FALSE(destination);
            destination = request;
            departureTick = tick;
            // Committing player travel does not restart the active portal loop.
            CHECK(sample() == beforeDeparture);
        }
    }
    REQUIRE(destination);
    CHECK(*destination == "mount1");
    CHECK(departureTick >= 120);
    CHECK(departureTick <= 123);
    CHECK(device.texturesCreated == texturesBefore);
    CHECK_FALSE(scene->tickPortals(visitors, 1));
}

TEST_CASE("Legends scene rejects invalid party counts before loading native assets",
          "[legends][portal][scene-adapter]") {
    test::FakeRenderDevice device;
    formats::LegendsMap map;
    for (const auto platform : {LegendsPlatform::N64, LegendsPlatform::Dreamcast}) {
        CHECK_THROWS_AS(LegendsSceneAdapter::load(device, platform, {}, map, 0),
                        std::invalid_argument);
        CHECK_THROWS_AS(LegendsSceneAdapter::load(device, platform, {}, map, 5),
                        std::invalid_argument);
    }
    CHECK(device.texturesCreated == 0);
}
} // namespace
