#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"
#include "engine/world/WorldCollision.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/bosses/LegendsDreamcastBossAssets.h"
#include "expansions/dreamcast/camera/LegendsDreamcastArenaCamera.h"
#include "expansions/dreamcast/formats/LegendsDreamcastCritter.h"
#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"
#include "expansions/shared/scenes/LegendsTraversal.h"

namespace gdl::game {
namespace {
CameraSubject subject(Vec3 feet, f32 height = 3) {
    return {feet, feet + Vec3{0, height, 0}, height};
}

bool finite(const Mat4& matrix) {
    for (Mat4::length_type column = 0; column < 4; ++column) {
        for (Mat4::length_type row = 0; row < 4; ++row) {
            if (!std::isfinite(matrix[column][row])) {
                return false;
            }
        }
    }
    return true;
}

TEST_CASE("Dreamcast BCAM fits the native arena before and after boss registration",
          "[legends][dreamcast-camera][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_regular_file(root / "levels/levele2/worlds.rom")) {
        SKIP("Dreamcast boss camera assets are not installed");
    }
    for (const auto& [level, realm, bossName] : std::array{
             std::array{"levelb6", "mount", "dragon"}, std::array{"levela5", "castle", "chimera"},
             std::array{"levelc5", "desert", "djinn"}, std::array{"leveld5", "forest", "drider"},
             std::array{"levele2", "temple", "skorne1"},
             std::array{"levelf2", "hell", "skorne2"}}) {
        DYNAMIC_SECTION(level) {
            const auto directory = root / "levels" / level;
            const auto worlds = readFile(directory / "worlds.rom");
            const auto nativeWorld = formats::WorldFile::parse(worlds);
            const auto scene = formats::LegendsDreamcastScene::parse(worlds);
            auto map =
                formats::LegendsMap::parseDreamcast(worlds, readFile(directory / "objects.rom"));
            test::FakeRenderDevice device;
            auto adapter =
                LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
            LegendsTraversal traversal;
            traversal.open(map, 0, CharacterSave{});
            REQUIRE(traversal.authoredStart());
            const auto metadata = formats::dreamcastLegendsFixtureScope(
                readFile(root / "worlddata" / (std::string{realm} + ".wad")), level);
            REQUIRE(metadata);
            REQUIRE(metadata->camera);
            REQUIRE(metadata->bossCamera);
            const auto start = std::ranges::find(nativeWorld.locators, LocatorKind::Start,
                                                 &formats::WorldLocatorRecord::kind);
            const auto bossLocator = std::ranges::find(nativeWorld.locators, LocatorKind::Boss,
                                                       &formats::WorldLocatorRecord::kind);
            REQUIRE(start != nativeWorld.locators.end());
            REQUIRE(bossLocator != nativeWorld.locators.end());
            const auto profile = legendsDreamcastBossCamera(
                readFile(root / "critters" / (std::string{bossName} + ".wad")));
            const auto types = formats::decodeLegendsDreamcastCritter(
                readFile(root / "critters" / (std::string{bossName} + ".wad")));
            REQUIRE(adapter->dreamcastCameraConfig());
            const auto& native = *adapter->dreamcastCameraConfig();
            if (!metadata->camera->authoredBounds) {
                CHECK(native.boundsMin == nativeWorld.minBounds + Vec3{8, 0, 8});
                CHECK(native.boundsMax == nativeWorld.maxBounds + Vec3{-8, 4, -8});
            }
            const CameraRange range{native.radiusMin, native.radiusMax, native.minPitch,
                                    native.boundsMin, native.boundsMax};
            Vec3 player = traversal.start();
            const auto& actor = traversal.actor();
            const auto playerSubject = [&](Vec3 feet) {
                return CameraSubject{feet, feet + actor.followPoint() - actor.position(),
                                     actor.height() * .5f};
            };
            std::array party{playerSubject(player)};
            BossCameraSubject boss;
            boss.position = bossLocator->position;
            boss.facing = bossLocator->rotation.y;
            boss.radius = types.types.front().radius;
            boss.attentionOffset = Vec3{0, profile.floorOffset + profile.attentionHeight, 0};
            boss.baseAttention = boss.position + Vec3{0, profile.floorOffset, 0};
            LegendsDreamcastArenaCamera at30;
            LegendsDreamcastArenaCamera at60;
            for (auto* camera : {&at30, &at60}) {
                camera->reset(boss, party, *metadata->bossCamera, range, scene.cameraMarkers);
            }
            const auto checkPlayer = [&] {
                const Vec4 clip = WorldCamera::projection(kPi / 3, 640.0f / 416) *
                                  at30.camera().view() * Vec4{player, 1};
                CAPTURE(clip.x, clip.y, clip.w, boss.engaged);
                REQUIRE(clip.w > 0);
                CHECK(std::abs(clip.x / clip.w) < 1);
                CHECK(std::abs(clip.y / clip.w) < 1);
            };
            // The true BCAM path runs even before BossRegister, after the authored
            // entrance pan. Its player range is independent of CAMS.radiusMax.
            checkPlayer();
            for (s32 tick = 0; tick < 300; ++tick) {
                at30.update(party, 1.0f / 30);
                at60.update(party, 1.0f / 60);
                at60.update(party, 1.0f / 60);
                CHECK(at30.camera() == at60.camera());
            }
            checkPlayer();
            // Dragon/Drider start outside their native TYPE wake radius. Test
            // active framing on a grounded point inside it, not an impossible
            // forced wake while the party is still at the distant entrance.
            Vec3 toPlayer = player - boss.position;
            toPlayer.y = 0;
            const f32 wakeDistance = types.types.front().wakeThreshold;
            if (wakeDistance > 0 && glm::length(toPlayer) >= wakeDistance) {
                Vec3 combatPoint{0};
                std::optional<FloorHit> combatFloor;
                // Some arenas have a central pit. Admit a real standing floor
                // along the approach, rather than inventing support at a fixed
                // radius inside the boss's wake region.
                for (s32 tenth = 9; tenth > 0 && !combatFloor; --tenth) {
                    const f32 combatDistance = wakeDistance * static_cast<f32>(tenth) * .1f;
                    combatPoint = boss.position + glm::normalize(toPlayer) * combatDistance;
                    combatFloor =
                        traversal.collision().floorAt(combatPoint, PlayerActor::kStepUp, 80);
                }
                REQUIRE(combatFloor);
                player = {combatPoint.x, combatFloor->y, combatPoint.z};
                party.front() = playerSubject(player);
                CHECK(glm::distance(Vec2{player.x, player.z},
                                    Vec2{boss.position.x, boss.position.z}) < wakeDistance);
            }
            boss.engaged = true;
            at30.trackBoss(boss);
            at60.trackBoss(boss);
            for (s32 tick = 0; tick < 600; ++tick) {
                at30.update(party, 1.0f / 30);
                at60.update(party, 1.0f / 60);
                at60.update(party, 1.0f / 60);
                CHECK(at30.camera() == at60.camera());
                CHECK(std::isfinite(at30.camera().pitch));
                const auto completed = at30.camera();
                for (const f32 fraction : {.0f, .25f, .75f, 1.0f}) {
                    // A frame between steps is drawn from the sample's own view.
                    const Mat4 view = at30.sample(fraction / 30).view();
                    CHECK(finite(WorldCamera::projection(80 * kPi / 180, 16.0f / 9) * view));
                    CHECK(finite(WorldCamera::projection(60 * kPi / 180, 4.0f / 3) * view));
                }
                CHECK(at30.camera() == completed);
            }
            checkPlayer();
            // In particular the former E2/F2 24/20-unit legacy view cut the
            // player off below the frame. Preserve the strict viewport check.
            CHECK(glm::distance(at30.camera().position, at30.attention()) >=
                  metadata->bossCamera->minDistance - .01f);
            at30.freeze();
            const auto held = at30.camera();
            at30.update(std::array{subject(player + Vec3{100, -100, 0})}, 1);
            CHECK(at30.camera() == held);
            CHECK(at30.sample() == held);
            CHECK(at30.sample(1.0f / 60) == held);
        }
    }
}

TEST_CASE(
    "Dreamcast boss presentation preserves native fitting and safely frames focus transitions",
    "[legends][dreamcast-camera][dreamcast-camera-safety]") {
    constexpr CameraView kView{kPi / 3, 640.0f / 416};
    const CameraRange range{20, 30, .35f, Vec3{-200}, Vec3{200}};
    const std::array party{CameraSubject{{.25f, -10, 23.25f}, {.25f, -7.5f, 23.25f}, 2.5f}};
    const std::array markers{
        WorldLocator{LocatorKind::CameraGame, 0, 0, {.25f, 0, 25.25f}, {25 * kPi / 180, kPi, 0}}};
    // F2's native BCAM attention transition, isolated from optional assets.
    BossCameraInfo record;
    record.flags = BossCameraInfo::kLiveAnchor;
    record.minDistance = 58;
    record.maxDistance = 75;
    record.minPlayerDistance = 35;
    record.maxPlayerDistance = 40;
    record.minPitch = 14 * kPi / 180;
    record.maxPitch = 8 * kPi / 180;
    record.minAttention = {0, -1, 0};
    record.maxAttention = {0, -8, 0};
    BossCameraSubject boss;
    boss.position = {0, 0, -10};
    boss.radius = 7.5f;
    boss.height = 0;
    BossCamera::ApproachPolicy approach;
    approach.maxDistanceScale = 1.5f;
    approach.inwardLooseMargin = 2.5f;
    approach.inwardHeldMargin = 2.25f;
    approach.inwardGain = 2;
    approach.boundsMidpoint = true;
    approach.attentionBounds = std::pair{range.boundsMin, range.boundsMax};
    BossCamera native;
    native.reset(boss, party, record, kView, markers, &approach);
    LegendsDreamcastArenaCamera presented;
    presented.reset(boss, party, record, range, markers);
    REQUIRE(presented.camera() == native.camera());
    const auto sideMargin = [&](const WorldCamera& camera, Vec3 point) {
        const Mat4 rows = glm::transpose(
            WorldCamera::projection(kView.horizontalFov, kView.aspect) * camera.view());
        f32 margin = std::numeric_limits<f32>::max();
        for (s32 axis = 0; axis < 2; ++axis) {
            for (const f32 sign : {-1.0f, 1.0f}) {
                const Vec4 plane = rows[3] + rows[axis] * sign;
                margin =
                    std::min(margin, glm::dot(plane, Vec4{point, 1}) / glm::length(Vec3{plane}));
            }
        }
        return margin;
    };
    bool corrected = false;
    bool unchanged = false;
    boss.engaged = true;
    presented.trackBoss(boss);
    for (s32 tick = 0; tick < 180; ++tick) {
        native.update(boss, party, record, kView, 1.0f / 30, markers, &approach);
        presented.update(party, 1.0f / 30);
        const auto camera = presented.camera();
        CHECK(camera.pitch == native.camera().pitch);
        CHECK(camera.yaw == native.camera().yaw);
        CHECK(presented.attention() == native.attention());
        const f32 nativeMargin =
            std::min(sideMargin(native.camera(), party[0].feet),
                     sideMargin(native.camera(), party[0].follow) - party[0].viewRadius);
        CHECK(sideMargin(camera, party[0].feet) >= 0);
        CHECK(sideMargin(camera, party[0].follow) >= party[0].viewRadius);
        if (nativeMargin > .01f) {
            unchanged = true;
            CHECK(camera == native.camera());
        } else if (nativeMargin < -.01f) {
            corrected = true;
            const Vec3 retreat = native.camera().position - camera.position;
            CHECK(glm::dot(retreat, native.camera().forward()) > 0);
            CHECK(glm::length(glm::cross(retreat, native.camera().forward())) < .0001f);
        }
    }
    CHECK(corrected);
    CHECK(unchanged);
}
} // namespace
} // namespace gdl::game
