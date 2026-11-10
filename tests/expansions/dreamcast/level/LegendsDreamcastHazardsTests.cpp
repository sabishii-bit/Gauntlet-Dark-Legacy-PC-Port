#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/dreamcast/formats/LegendsSceneMotion.h"
#include "expansions/dreamcast/level/LegendsDreamcastHazards.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"
#include "expansions/shared/scenes/LegendsTraversal.h"
#include "game/enemies/Enemies.h"
#include "game/level/HazardSurfaces.h"
#include "game/level/LevelWorld.h"
#include "game/party/PlayerHealth.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Dreamcast harmful surfaces inherit only native contact-policy flags",
          "[legends][dreamcast-hazards]") {
    formats::LegendsMap map;
    map.objects.resize(5);
    // Parents need not precede children. Unrelated rendering/motion/collision
    // bits must not become common engine flags just because their values overlap.
    map.objects[0].parent = 3;
    map.objects[0].flags = 0x01003002;
    map.objects[3].parent = 1;
    map.objects[3].flags = 0x02000000;
    map.objects[1].flags = 0x00030000;
    map.objects[2].flags = 0x00060000;
    map.objects[4].parent = 0;
    map.objects[4].flags = 0x08000000;
    const auto flags = dreamcastSurfaceHazardFlags(map);
    CHECK(flags == std::vector<u32>{0x02030000, 0x00030000, 0x00060000, 0x02030000, 0x0A030000});
    HazardSurfaces hazards;
    hazards.bindFlags(flags);
    CHECK_FALSE(hazards.harmOfObject(0));
    REQUIRE(hazards.harmOfObject(4));
    CHECK(hazards.harmOfObject(4)->damage == 15);
    CHECK(hazards.harmOfObject(4)->impact == damage::kKnockDown);
    REQUIRE(hazards.harmOfObject(2));
    CHECK(hazards.harmOfObject(2)->damage == 5);
    CHECK_FALSE(HazardSurfaces::enemyHarmOf(flags[2]));
    CHECK(dreamcastSurfaceHazardFlags({}).empty());
}

TEST_CASE("Dreamcast harmful surface ancestry rejects malformed or excessive graphs",
          "[legends][dreamcast-hazards]") {
    formats::LegendsMap map;
    map.objects.resize(3);
    map.objects[0].parent = 2;
    map.objects[2].parent = 1;
    map.objects[1].parent = 0;
    CHECK_THROWS_AS(dreamcastSurfaceHazardFlags(map), FormatError);
    map.objects[1].parent = 3;
    CHECK_THROWS_AS(dreamcastSurfaceHazardFlags(map), FormatError);
    map.objects[1].parent = -2;
    CHECK_THROWS_AS(dreamcastSurfaceHazardFlags(map), FormatError);
    map.objects[1].parent = -1;
    CHECK_NOTHROW(dreamcastSurfaceHazardFlags(map));
    // A deep but valid chain is bounded and iterative, not a call-stack walk.
    map.objects.resize(16384);
    for (usize i = 0; i < map.objects.size(); ++i) {
        map.objects[i].parent = i + 1 == map.objects.size() ? -1 : static_cast<s32>(i + 1);
    }
    map.objects.back().flags = 0x00020000;
    const auto flags = dreamcastSurfaceHazardFlags(map);
    CHECK(flags.front() == 0x00020000);
    CHECK(flags.back() == 0x00020000);
    map.objects.emplace_back();
    CHECK_THROWS_AS(dreamcastSurfaceHazardFlags(map), FormatError);
}

TEST_CASE("Dreamcast shipped harmful surface census is retained without name-specific rules",
          "[legends][dreamcast-hazards][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels";
    if (!std::filesystem::is_directory(directory)) {
        SKIP("Optional Dreamcast extraction is not installed");
    }
    std::array<usize, 16> kinds{};
    usize levels = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto path = entry.path() / "worlds.rom";
        if (!std::filesystem::is_regular_file(path)) {
            continue;
        }
        const auto bytes = readFile(path);
        // The extracted disc includes a zero-byte levels/test placeholder,
        // not another playable world revision. All 41 actual worlds are v2.
        if (bytes.empty()) {
            CHECK(entry.path().filename() == "test");
            continue;
        }
        const auto motion = formats::LegendsSceneMotion::parseDreamcast(bytes);
        formats::LegendsMap map;
        map.objects.resize(motion.objects.size());
        for (usize i = 0; i < map.objects.size(); ++i) {
            map.objects[i].flags = motion.objects[i].flags;
            map.objects[i].parent = motion.parents[i];
        }
        const auto flags = dreamcastSurfaceHazardFlags(map);
        for (usize i = 0; i < flags.size(); ++i) {
            if (motion.objects[i].collisionTriangleCount <= 0 || (flags[i] & 0xF0000) == 0) {
                continue;
            }
            ++kinds[(flags[i] >> 16) & 15];
            CHECK((flags[i] & 0x0A000000) == 0);
            CHECK(HazardSurfaces::harmOf(flags[i]).has_value());
        }
        ++levels;
    }
    CHECK(levels == 41);
    CHECK(kinds == std::array<usize, 16>{0, 9, 11, 20, 0, 0, 152, 4, 14, 0, 0, 0, 0, 0, 0, 0});
}

/** Locate a standing cylinder touching a real, currently transformed native
 * triangle. No object-centre distance test or replacement collision is used. */
std::optional<Vec3> standingContact(const formats::LegendsMap& map, const LevelWorld& world,
                                    s32 owner) {
    const Mat4 transform = world.collision().objectTransform(owner).value_or(Mat4{1});
    for (const auto& face : map.collision) {
        if (face.owner != owner) {
            continue;
        }
        const Vec3 normal = glm::normalize(Vec3{transform * Vec4{face.normal, 0}});
        // The mill's long triangles have centroids far above the walkway,
        // even while their outer edges cross it. Sample the whole native face,
        // then require both actual cylinders to touch the unchanged mesh.
        constexpr s32 kDivisions = 8;
        for (s32 u = 0; u <= kDivisions; ++u) {
            for (s32 v = 0; v <= kDivisions - u; ++v) {
                const f32 a = static_cast<f32>(u) / kDivisions;
                const f32 b = static_cast<f32>(v) / kDivisions;
                const Vec3 point =
                    Vec3{transform * Vec4{face.vertices[0] * (1 - a - b) + face.vertices[1] * a +
                                              face.vertices[2] * b,
                                          1}};
                for (const f32 offset : {.2f, .6f, -.2f}) {
                    Vec3 feet = point + normal * offset;
                    const auto floor = world.collision().floorAt(feet, 1, 6);
                    if (!floor) {
                        continue;
                    }
                    feet.y = floor->y;
                    const auto playerTouch =
                        world.hazards().touching(world.collision(), feet, .75f, 5);
                    const auto enemyTouch = world.hazards().touching(world.collision(), feet,
                                                                     enemyKind(kGruntKind).radius,
                                                                     enemyKind(kGruntKind).height);
                    if (playerTouch && enemyTouch && playerTouch->object == owner &&
                        enemyTouch->object == owner) {
                        return feet;
                    }
                }
            }
        }
    }
    return {};
}

TEST_CASE("Dreamcast moving boulders and mill blades reach common player and enemy damage",
          "[legends][dreamcast-hazards][assets]") {
    const auto levels =
        test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native/levels";
    if (!std::filesystem::is_regular_file(levels / "levelb3/worlds.rom") ||
        !std::filesystem::is_regular_file(levels / "levelg1/worlds.rom")) {
        SKIP("Optional Dreamcast extraction is not installed");
    }
    const auto common =
        test::assetOrSkip("MONSTERS/GRU/ANIM.PS2").parent_path().parent_path().parent_path();
    struct Case {
        const char* level;
        const char* name;
        s32 owner;
        f32 playerDamage;
        f32 enemyDamage;
        u32 impact;
    };
    for (const Case sample : {Case{"levelb3", "B3NS_TMBL#0", 56, 15, 15, damage::kKnockDown},
                              Case{"levelg1", "G1MILL_BLADES", 637, 10, 5, damage::kKnockBack}}) {
        CAPTURE(sample.level, sample.owner);
        const auto directory = levels / sample.level;
        const auto bytes = readFile(directory / "worlds.rom");
        auto map = formats::LegendsMap::parseDreamcast(bytes, readFile(directory / "objects.rom"));
        REQUIRE(map.objects.at(static_cast<usize>(sample.owner)).name == sample.name);
        test::FakeRenderDevice device;
        auto adapter =
            LegendsSceneAdapter::load(device, LegendsPlatform::Dreamcast, directory, map);
        REQUIRE(adapter);
        LegendsTraversal traversal;
        traversal.open(map, 0, CharacterSave{});
        auto native =
            std::make_shared<LegendsLevel>(device, map, std::move(adapter), directory, common);
        LevelWorld world;
        LevelRef reference;
        reference.realmId = -1;
        reference.name = std::string{"dreamcast:"} + sample.level;
        REQUIRE(world.loadImported(device, common, reference,
                                   WorldLayout::fromDecoded({}, {}, map.minBounds, map.maxBounds),
                                   traversal.collision(), native));
        REQUIRE(world.collision().moving(sample.owner));
        REQUIRE(world.hazards().harmOfObject(sample.owner));
        CHECK(world.hazards().harmOfObject(sample.owner)->damage == sample.playerDamage);
        CHECK(world.hazards().harmOfObject(sample.owner)->impact == sample.impact);
        const Mat4 start = *world.collision().objectTransform(sample.owner);
        usize contacts = 0;
        bool moved = false;
        for (s32 sampleFrame = 0; sampleFrame < 60; ++sampleFrame) {
            if (sampleFrame != 0) {
                for (s32 tick = 0; tick < 10; ++tick) {
                    world.capturePresentation();
                    world.update(1.f / 30);
                }
            }
            moved |= *world.collision().objectTransform(sample.owner) != start;
            const auto at = standingContact(map, world, sample.owner);
            if (!at) {
                continue;
            }
            CAPTURE(sampleFrame, at->x, at->y, at->z);
            const auto touch = world.hazards().touching(world.collision(), *at, .75f, 5);
            REQUIRE(touch);
            REQUIRE(touch->object == sample.owner);
            auto visibility = world.collision();
            visibility.setSolid(sample.owner, false);
            const auto hidden = world.hazards().touching(visibility, *at, .75f, 5);
            // Other legitimate hazards (such as the lava below the boulder)
            // may remain; this removed mesh must no longer deal damage.
            CHECK((!hidden || hidden->object != sample.owner));
            visibility.setSolid(sample.owner, true);
            const auto restored = world.hazards().touching(visibility, *at, .75f, 5);
            REQUIRE(restored);
            CHECK(restored->object == sample.owner);
            PlayerRuntime player;
            player.actor.spawn(0, {}, nullptr, *at, 0);
            player.actor.save().progress().health = 1000;
            usize impactSounds = 0;
            const PlayerHealth::Events events{.block = [](f32, f32) {},
                                              .sound = [&](std::string_view) { ++impactSounds; },
                                              .cry = [](std::string_view) {},
                                              .named = [](std::string_view, f32) {},
                                              .learnBlock = [] {},
                                              .vibrate = {}};
            PlayerHealth health;
            health.hurt(player, touch->harm.damage, HurtKind::Blow, HurtSource::Directed, {},
                        events, {touch->harm.impact, touch->away});
            CHECK(player.actor.save().health() == 1000 - static_cast<s32>(sample.playerDamage));
            CHECK(player.reaction ==
                  PlayerImpact{sample.impact, touch->away}.reaction(sample.playerDamage, 0, false));
            CHECK(impactSounds == 1);

            Enemies enemies;
            enemies.open(device, common, &world.collision(), 13, {}, 1);
            enemies.setHazards(&world.hazards());
            REQUIRE(enemies.loadKind(kGruntKind));
            EnemySpawn spawn;
            spawn.placed = true;
            spawn.tier = 3;
            spawn.position = *at;
            const auto id = enemies.spawn(spawn, {});
            REQUIRE(id);
            const f32 before = enemies.healthOf(*id);
            enemies.update(2, 1.f / 30, {});
            CHECK(enemies.healthOf(*id) == before - sample.enemyDamage);
            CHECK(enemies.takeLosses().empty());
            enemies.close();
            ++contacts;
            if (contacts >= 2 && moved) {
                break;
            }
        }
        CHECK(moved);
        CHECK(contacts >= 2);
    }
}
} // namespace
