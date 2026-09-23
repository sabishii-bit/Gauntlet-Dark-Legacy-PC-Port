#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "formats/CritterWad.h"
#include "game/enemies/BossDefinition.h"
#include "game/world/CombatantProjectiles.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

struct Fixture {
    test::FakeRenderDevice device;
    ItemArchive archive;
    CritterData data;
    EffectTrees effects;
    CombatantProjectiles projectiles;
    std::vector<std::string> sounds;
    CombatantProjectiles::PlaySound sound = [&](std::string_view name) { sounds.emplace_back(name); };
    Fixture() {
        const auto root = test::scratchDirectory("critter-projectiles");
        writeTextFile(root / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        writeFile(root / "white.png", test::kTinyPng);
        writeTextFile(root / "objects.json", R"({"objects":[{"name":"TRI","file":"tri.obj"}]})");
        writeTextFile(root / "textures.json",
                      R"({"bitmaps":[{"name":"WHITE","file":"white.png","width":2,"height":2}]})");
        writeTextFile(root / "animations.json", R"({"trees":[
            {"name":"SHOT","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}],"sequences":[{"name":"BIRTH","frames":3,"frameRate":30}]},
            {"name":"LOOP","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}]},
            {"name":"HIT","nodes":[{"name":"ROOT","object":"TRI","parent":-1,"position":[0,0,0]}]}]})");
        writeTextFile(root / "critter.json",
                      R"({"name":"TEST","types":[{"moveCount":1}],"descriptors":[{}],"moves":[{}],
            "damages":[{"type":1,"flags":32,"behaviorFlags":9,"radius":0.5,"damage":12,"minSpeed":30,"maxSpeed":30,
                "sfxIndex":0,"sfx":2,"morph":1,"morphEnd":2,"morphLife":0.5}],
            "sounds":[{"name":"SHOT","levelFormat":"S_%cSHOT"},{"name":"LOOP"},{"name":"HIT","levelFormat":"S_%cHIT"}]})");
        REQUIRE(archive.load(root));
        REQUIRE(data.load(root / "critter.json"));
    }
    void launch() {
        CombatShot shot;
        shot.data = &data;
        shot.damageIndex = 0;
        shot.origin = {0, 3, 0};
        shot.target = Vec3{0, 3, 30};
        shot.realm = 'C';
        shot.damageScale = 2;
        projectiles.launch(shot, archive, device, effects, sound);
    }
    void step(f32 seconds, std::span<const EnemyView> players = {},
              const WorldCollision* collision = nullptr) {
        effects.update(seconds);
        projectiles.update(seconds, collision, players, device, effects, sound);
    }
};

TEST_CASE("critter projectiles move birth effects morph expire and clear their borrowed effects",
          "[game][boss-projectiles]") {
    Fixture f;
    f.launch();
    REQUIRE(f.projectiles.count() == 1);
    REQUIRE(f.sounds == std::vector<std::string>{"S_CSHOT"});
    f.step(0.04f);
    REQUIRE(f.effects.effect(0).position.z == Approx(1.2f));
    f.step(0.05f);
    REQUIRE(f.effects.effect(0).name == "LOOP");
    f.step(0.51f);
    REQUIRE(f.projectiles.count() == 0);
    REQUIRE(f.effects.effect(0).name == "HIT");
    REQUIRE(f.sounds.back() == "S_CHIT");
    f.effects.clear();
    f.launch();
    f.projectiles.clear(f.effects);
    REQUIRE(f.projectiles.count() == 0);
    REQUIRE(f.effects.count() == 0);
    REQUIRE(f.projectiles.takeHits().empty());
}

TEST_CASE("critter projectiles choose the nearest visible victim regardless of party order",
          "[game][boss-projectiles]") {
    Fixture f;
    f.launch();
    const std::vector<EnemyView> players{EnemyView{7, {0, 0, 5}, 1, 6},
                                         EnemyView{3, {0, 0, 3}, 1, 6},
                                         EnemyView{9, {0, 0, 0}, 1, 6, 1, true}};
    f.step(0.2f, players);
    const auto hits = f.projectiles.takeHits();
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].player == 3);
    REQUIRE(hits[0].damage == 24);
    REQUIRE(hits[0].flags == 0x20);
    REQUIRE(hits[0].direction == Vec3(0, 0, 1));
    REQUIRE(f.projectiles.count() == 0);
    REQUIRE(f.effects.effect(0).name == "HIT");
    REQUIRE(f.effects.effect(0).position.z == Approx(1.5f));
    f.step(0.1f, players);
    REQUIRE(f.projectiles.takeHits().empty());
}

TEST_CASE("critter projectiles cannot damage players through a world wall",
          "[game][boss-projectiles]") {
    Fixture f;
    WorldCollision world;
    CollisionTriangle wall;
    wall.object = 1;
    wall.normal = {0, 0, -1};
    wall.vertices = {Vec3{-10, 0, 2}, Vec3{0, 20, 2}, Vec3{10, 0, 2}};
    world.build({wall});
    f.launch();
    const std::vector<EnemyView> players{EnemyView{0, {0, 0, 5}, 1, 6}};
    f.step(0.2f, players, &world);
    REQUIRE(f.projectiles.count() == 0);
    REQUIRE(f.projectiles.takeHits().empty());
    REQUIRE(f.effects.effect(0).name == "HIT");
}
TEST_CASE("retail boss projectile records retain physics and effect transitions",
          "[game][boss-projectiles][assets][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/DRIDER.json").parent_path().parent_path();
    for (s32 kind = 34; kind <= 44; ++kind) {
        const std::string name{bossNameOf(kind)};
        DYNAMIC_SECTION(name) {
            const auto wad = test::assetOrSkip("CRITTER/" + name + ".WAD");
            test::unpackedOrSkip("MONSTERS/" + name + "/animations.json");
            const auto raw = formats::parseCritterWad(readFile(wad));
            CritterData data;
            REQUIRE(data.load(root / "critter" / (name + ".json")));
            REQUIRE(data.damages().size() == raw.damages.size());
            ItemArchive archive;
            REQUIRE(archive.load(root / "MONSTERS" / name));
            test::FakeRenderDevice device;
            EffectTrees effects;
            CombatantProjectiles projectiles;
            usize launched = 0;
            for (usize index = 0; index < data.damages().size(); ++index) {
                const AttackDefinition& damage = data.damages()[index];
                if (damage.type != AttackDefinition::kProjectile || damage.sound < 0) {
                    continue;
                }
                CAPTURE(name, index);
                REQUIRE(damage.behaviorFlags == static_cast<u16>(raw.damages[index].behaviorFlags));
                REQUIRE(damage.gravity == raw.damages[index].gravity);
                REQUIRE(damage.maxSpeed == raw.damages[index].maxSpeed);
                REQUIRE(damage.morph == raw.damages[index].morph);
                REQUIRE(damage.morphEnd == raw.damages[index].morphEnd);
                REQUIRE(damage.morphLife == raw.damages[index].morphLife);
                REQUIRE(damage.yawSpread == raw.damages[index].yawSpread);
                CombatShot shot;
                shot.data = &data;
                shot.damageIndex = static_cast<s32>(index);
                shot.origin = {0, 30, 0};
                shot.target = Vec3{0, 5, 60};
                projectiles.launch(shot, archive, device, effects, {});
                REQUIRE(projectiles.count() == 1);
                REQUIRE(effects.count() >= 1);
                projectiles.update(1.0f / 30, nullptr, {}, device, effects, {});
                effects.update(1.0f / 30);
                projectiles.clear(effects);
                REQUIRE(projectiles.count() == 0);
                ++launched;
            }
            if (name != "LICH") {
                REQUIRE(launched > 0);
            }
        }
    }
}
} // namespace
