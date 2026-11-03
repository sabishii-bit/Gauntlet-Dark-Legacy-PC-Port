#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "game/level/ScriptedFixtures.h"
#include "game/party/PlayerAttacks.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct ArenaCombatFixture {
    test::FakeRenderDevice device;
    ClassDataSet classes;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    AmbientDimmer dimmer;
    PlayerArsenal arsenal;
    LevelOpponents opponents;
    LevelFixtures fixtures;
    PlayerAttacks attacks;
    std::array<PlayerRuntime, 1> players;
    test::ScriptedFixtures fixtureHost;
    PlayerAttacks::Targets targets{opponents, fixtures, fixtureHost};
    std::shared_ptr<LegendsLevel> native;
    Vec3 cover{0};

    ArenaCombatFixture() {
        const auto directory =
            test::dataDirectory().parent_path() / "assets/n64/extracted/levels/mount6";
        if (!std::filesystem::is_regular_file(directory / "worlds.wad")) {
            SKIP("Extracted N64 Legends assets unavailable");
        }
        const auto root = test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
        const auto models = readFile(directory / "objects.wad");
        auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"), models,
                                                 readFile(directory / "ipr.rom"));
        const auto dynamic = std::filesystem::is_regular_file(directory / "texdyn.rom")
                                 ? readFile(directory / "texdyn.rom")
                                 : std::vector<u8>{};
        for (auto& texture : formats::decodeLegendsN64Textures(models, dynamic)) {
            map.textures.push_back({std::move(texture.image), texture.clampU, texture.clampV,
                                    texture.hasTransparency, std::move(texture.name),
                                    texture.external, texture.width, texture.height});
        }
        auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
        native = std::make_shared<LegendsLevel>(device, map, std::move(adapter), directory, root);
        REQUIRE(native->arenaMissileStops().size() == 6);
        cover = native->arenaMissileStops().front().box.centre;

        // Keep the authored arena bodies/art, but isolate combat admission from
        // native floor/wall traversal and the active Dragon encounter.
        WorldCollision collision;
        const Vec3 a = cover + Vec3{-100, 0, -100};
        const Vec3 b = cover + Vec3{-100, 0, 100};
        const Vec3 c = cover + Vec3{100, 0, -100};
        const Vec3 d = cover + Vec3{100, 0, 100};
        collision.build({{{0, 1, 0}, {a, b, c}, 0, 4}, {{0, 1, 0}, {c, b, d}, 0, 4}});
        auto layout = WorldLayout::fromDecoded({}, {}, a, d + Vec3{0, 30, 0});
        LevelRef ref;
        ref.realmId = -1;
        ref.realm = "Legends";
        ref.name = "n64:mount6";
        const bool loaded =
            world.loadImported(device, root, ref, std::move(layout), std::move(collision), native);
        REQUIRE(loaded);
        REQUIRE(classes.load(root / "PDATA"));
        const auto* stats = classes.stats(0);
        REQUIRE(stats);
        arsenal.bind({device, classes, weapons, world.collision(), effects, audio, nullptr, {}});
        attacks.bind({device, classes, world, weapons, effects, audio, nullptr, arsenal, dimmer});
        fixtures.bind({device, world, weapons, effects, audio});
        players[0].actor.spawn(0, {}, stats, cover + Vec3{0, 0, -4}, 0);
    }

    s32 health() const {
        const auto stops = native->arenaMissileStops();
        const auto first = std::ranges::find(stops, kImportedArenaBase, &MissileStop::rock);
        return first == stops.end() ? 0 : first->rockHealth;
    }
};
} // namespace

TEST_CASE("Native arena cover remains a physical target without acquiring the player's aim",
          "[legends][arena-combat][assets]") {
    ArenaCombatFixture f;
    const auto bodies = f.native->targets();
    const auto first = std::ranges::find(bodies, kImportedArenaBase, &ImportedTarget::id);
    REQUIRE(first != bodies.end());
    CHECK(first->valid());
    CHECK(first->damageable);
    CHECK(first->magicImmune);
    REQUIRE(f.health() == 120);
    MissileTarget control{kImportedTargetBase + first->id, first->base, first->radius,
                          first->height};
    control.acquisition =
        TargetAssist::itemAcquisition(glm::translate(Mat4{1}, first->base), Vec3{0}, first->radius,
                                      first->height, TargetAssist::kItemDistanceScale);
    REQUIRE(TargetAssist::select(f.players[0].actor.followPoint(), Vec3{0, 0, 1},
                                 std::span{&control, 1}, TargetAssist::kRange,
                                 &f.world.collision()));
    const auto acquisition = f.attacks.meleeAcquisition(f.players[0].actor, f.targets);
    REQUIRE(acquisition);
    for (const auto& body : bodies) {
        if (body.id >= kImportedArenaBase) {
            CHECK(std::ranges::none_of(acquisition->targets, [&](const auto& target) {
                return glm::distance(target.base, body.base) < .001f;
            }));
        }
    }
    const auto aimed = f.attacks.aim(f.players[0].actor, Vec3{0, 0, 1}, f.targets);
    if (aimed) {
        // Other authored items may remain targetable; none of the safe rocks may
        // redirect a throw. Compare the actual item acquisition points.
        for (const auto& body : bodies) {
            if (body.id < kImportedArenaBase) {
                continue;
            }
            const auto point = TargetAssist::itemAcquisition(glm::translate(Mat4{1}, body.base),
                                                             Vec3{0}, body.radius, body.height,
                                                             TargetAssist::kItemDistanceScale);
            CHECK(glm::distance(*aimed, point.point) > .001f);
        }
    }

    static constexpr MissileSpec kSpec{"TEST", {}, .1f, 0, 0, true};
    MissileLaunch shot;
    shot.spec = &kSpec;
    shot.owner = 0;
    shot.position = f.cover + Vec3{0, 2, -4};
    shot.velocity = {0, 0, 30};
    shot.damage = 17;
    REQUIRE(f.arsenal.missiles().launch(shot));
    for (s32 tick = 0; tick < 30 && f.health() == 120; ++tick) {
        f.attacks.updateProjectiles(1.0f / 60, f.players, f.targets);
    }
    CHECK(f.health() == 103);
    CHECK(f.native->arenaTargets().front().active);
}

TEST_CASE("Native arena cover survives real potion shields and expanding potion waves",
          "[legends][arena-combat][assets]") {
    ArenaCombatFixture f;
    const auto health = f.health();
    REQUIRE(health == 120);
    SECTION("potion ward across repeated damage intervals") {
        auto& inventory = f.players[0].actor.save().progress().inventory;
        inventory.addPotions(1, 1);
        f.attacks.shieldPotion(0, f.players);
        REQUIRE(inventory.potions.empty());
        REQUIRE(f.attacks.shieldCount() == 1);
        for (s32 tick = 0; tick < 180; ++tick) {
            f.attacks.updateShields(1.0f / 60, f.players, f.targets);
        }
    }
    SECTION("a shattered potion centred directly on cover") {
        f.attacks.shatterPotion(1, f.cover);
        REQUIRE(f.attacks.pendingGameplay());
        for (s32 tick = 0; tick < 120; ++tick) {
            f.attacks.updateProjectiles(1.0f / 60, f.players, f.targets);
        }
        CHECK_FALSE(f.attacks.pendingGameplay());
    }
    CHECK(f.health() == health);
    CHECK(f.native->arenaTargets().front().active);
    CHECK(f.native->arenaMissileStops().size() == 6);
}
