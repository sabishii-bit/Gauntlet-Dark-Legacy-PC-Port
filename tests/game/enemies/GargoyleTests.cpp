#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/world/WorldCollision.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/Combatant.h"
#include "game/enemies/Critters.h"
#include "game/enemies/Gargoyle.h"
#include "game/world/CombatantProjectiles.h"
#include "game/world/EffectTrees.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr s32 kTicks = 2;
constexpr f32 kStep = 1.0f / 30.0f;

EnemyView playerAt(const Vec3& position) {
    EnemyView view;
    view.player = 0;
    view.position = position;
    view.radius = 1.0f;
    view.height = 6.0f;
    return view;
}

CollisionTriangle triangle(const Vec3& a, const Vec3& b, const Vec3& c) {
    CollisionTriangle out;
    out.vertices = {a, b, c};
    out.normal = Vec3{0.0f, 1.0f, 0.0f};
    out.object = 0;
    return out;
}

TEST_CASE("the gargoyle's family names its form and its key, and stands as a statue first",
          "[game][enemies][gargoyle]") {
    const CombatantDefinition eagle = Gargoyle::definition();
    REQUIRE(eagle.name == "GAR_EAGL");
    REQUIRE(eagle.dropForm == "EAGL");
    REQUIRE(eagle.kind == CombatantKind::Gargoyle);
    REQUIRE(eagle.breaksItems);
    REQUIRE_FALSE(eagle.patrols);
    REQUIRE(Gargoyle::definition("gar_lion").dropForm == "LION");
}

TEST_CASE("gargoyle death skins follow the death cue and the sack waits for the final pose",
          "[game][enemies][gargoyle][critter-death][unpacked]") {
    const auto* form = GENERATE("GAR_EAGL", "GAR_LION", "GAR_SERP");
    const auto root = test::unpackedOrSkip("critter/GAR_EAGL.json").parent_path().parent_path();
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, Gargoyle::definition(form), 'B'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, Vec3{0}, 0, nullptr, {}, 'B'));
    const auto death = assets.data.moveNamed("DEATH");
    REQUIRE(death.has_value());
    const auto& move = assets.data.moves()[*death];
    REQUIRE(move.soundFrame == 2);
    const auto* skin = assets.data.sound(move.sound);
    REQUIRE(skin != nullptr);
    REQUIRE(skin->tree == "DEATHGAR");
    REQUIRE(skin->flags == CombatEffectDefinition::kSkin);
    REQUIRE(assets.skins.at("DEATHGAR").size() == 15);
    EnemyHit hit;
    hit.player = 0;
    hit.damage = 100000;
    actor.hurt(hit);
    const auto initial = actor.takeLosses();
    REQUIRE(std::ranges::any_of(initial, &CombatLoss::killed));
    REQUIRE(std::ranges::none_of(initial, &CombatLoss::drop));
    bool sawSkin = false;
    bool sawCry = false;
    s32 skinSteps = 0;
    for (s32 step = 0; step < 60 && actor.present(); ++step) {
        actor.update(kTicks, kStep, {});
        for (const auto& cue : actor.takeCues()) {
            if (cue.sound == "S_GRGBDEATH") {
                REQUIRE(cue.tree.empty()); // SFXX names a skin, not an effect tree.
                REQUIRE_FALSE(cue.attenuated);
                sawCry = true;
            }
        }
        if (!actor.present()) {
            const auto drops = actor.takeLosses();
            REQUIRE(drops.size() == 1);
            REQUIRE(drops[0].drop);
            REQUIRE_FALSE(drops[0].killed);
            REQUIRE(drops[0].experience == 0);
            REQUIRE(step < 30); // no added second-long fade after the authored fall
            break;
        }
        REQUIRE(actor.takeLosses().empty());
        device.draws.clear();
        actor.draw(device, Mat4{1}, {});
        REQUIRE_FALSE(device.draws.empty());
        for (const auto& draw : device.draws) {
            if (sawCry) {
                REQUIRE(draw.state.maskedTexture ==
                        assets.skins.at("DEATHGAR")[static_cast<usize>(skinSteps / 2)]);
                sawSkin = true;
            } else {
                REQUIRE(draw.state.maskedTexture == nullptr);
            }
        }
        if (sawCry) {
            ++skinSteps;
        }
    }
    REQUIRE_FALSE(actor.present());
    REQUIRE(sawSkin);
    REQUIRE(sawCry);
    // Another fighter sharing the species must never inherit its corpse's skin.
    REQUIRE(actor.spawn(assets, 0, Vec3{0}, 0, nullptr, {}, 'B'));
    device.draws.clear();
    actor.draw(device, Mat4{1}, {});
    for (const auto& draw : device.draws) {
        REQUIRE(draw.state.maskedTexture == nullptr);
    }
}

TEST_CASE("skin cues and death completion run without retail assets", "[gargoyle][critter-death]") {
    const auto root = test::scratchDirectory("gargoyle-skin");
    const auto archive = root / "MONSTERS/GAR_EAGL";
    std::filesystem::create_directories(root / "critter");
    std::filesystem::create_directories(archive / "models");
    std::filesystem::create_directories(archive / "textures");
    writeTextFile(archive / "models/body.obj",
                  "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\nusemtl tex0\nf 1//1 2//1 3//1\n");
    writeTextFile(archive / "objects.json", R"({"objects":[
      {"index":0,"name":"BODY","file":"models/body.obj","meshTriangles":1}]})");
    writeFile(archive / "textures/skin.png", test::kTinyPng);
    writeTextFile(archive / "textures.json", R"({"bitmaps":[
      {"index":0,"name":"SKIN","file":"textures/skin.png","width":2,"height":2},
      {"index":1,"name":"FADE0","file":"textures/skin.png","width":2,"height":2},
      {"index":2,"name":"FADE1","file":"textures/skin.png","width":2,"height":2}]})");
    writeTextFile(archive / "animations.json", R"({"textureAnimations":[
      {"name":"FADE","texture":0,"source":1,"frames":2,"rate":2,"flag":-1}],
      "trees":[{"name":"GAR1","nodes":[
      {"name":"BODY","object":"BODY","parent":-1,"position":[0,0,0]}],
      "sequences":[{"name":"READY","frames":2,"frameRate":30},
      {"name":"DEATH","frames":8,"frameRate":30}]}]})");
    writeTextFile(root / "critter/GAR_EAGL.json", R"({"name":"GAR_EAGL",
      "descriptors":[{"name":"gar","prefix":"GAR","type":7}],
      "types":[{"suffix":"1","maxHealth":10,"moveBase":0,"moveCount":2}],
      "moves":[{"name":"READY","anim":"READY","type":32,"priority":1},
      {"name":"DEATH","anim":"DEATH","type":17,"priority":4095,
       "sfx":0,"sfxFrame":2}],
      "sounds":[{"name":"FADE","flags":256,"life":0.066667,"rate":0.5,"custom0":1}]})");
    test::FakeRenderDevice device;
    CombatantAssets assets;
    REQUIRE(assets.load(device, root, Gargoyle::definition(), 'B'));
    Combatant actor;
    REQUIRE(actor.spawn(assets, 0, Vec3{0}, 0, nullptr, {}, 'B'));
    EnemyHit hit;
    hit.damage = 1000;
    hit.player = 0;
    actor.hurt(hit);
    actor.takeLosses();
    for (s32 tick = 1; tick <= 8; ++tick) {
        actor.update(2, kStep, {});
        REQUIRE(actor.takeCues().empty()); // never emit FADE as a model effect
        if (tick < 8) {
            REQUIRE(actor.present());
            REQUIRE(actor.takeLosses().empty());
            device.draws.clear();
            actor.draw(device, Mat4{1}, {});
            REQUIRE(device.draws.size() == 1);
            const Texture* expected =
                tick < 2 ? nullptr
                         : assets.skins.at("FADE")[static_cast<usize>(((tick - 2) / 2) % 2)];
            REQUIRE(device.draws[0].state.maskedTexture == expected);
        } else {
            REQUIRE_FALSE(actor.present());
            const auto drops = actor.takeLosses();
            REQUIRE(drops.size() == 1);
            REQUIRE(drops[0].drop);
        }
    }
}

TEST_CASE("the eagle gargoyle breathes fire on a player near and throws its fireball at one "
          "further off, which flies to them and bursts",
          "[game][enemies][gargoyle][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/GAR_EAGL.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/GAR_EAGL/animations.json");
    test::FakeRenderDevice device;
    WorldCollision collision;
    collision.build({triangle({-100, 0, -100}, {100, 0, -100}, {100, 0, 100}),
                     triangle({-100, 0, -100}, {100, 0, 100}, {-100, 0, 100})});
    Critters critters;
    critters.open(device, root, &collision, EnemyScales{}, 'A');
    const auto id = critters.spawn(CombatantKind::Gargoyle, Vec3{0.0f, 0.0f, 0.0f}, 0.0f);
    REQUIRE(id.has_value());
    const CritterData& data = *critters.dataOf(*id);
    // Its table: the fireball is a projectile of twenty from ten to fifty, thrown at a player
    // squarely ahead, riding FBALL_LOOP and bursting as FBALL_EXP; the breath reaches to
    // twenty.
    const auto fireball = data.moveNamed("FBALL");
    REQUIRE(fireball.has_value());
    const MoveDefinition& throwing = data.moves()[*fireball];
    REQUIRE(throwing.type == 132);
    REQUIRE(throwing.target.minDistance == 10.0f);
    REQUIRE(throwing.target.maxDistance == 50.0f);
    REQUIRE(throwing.target.minDot == Approx(0.866025f));
    const AttackDefinition* ball = data.damage(throwing.damage0);
    REQUIRE(ball != nullptr);
    REQUIRE(ball->type == AttackDefinition::kProjectile);
    REQUIRE(ball->damage == 20.0f);
    REQUIRE(ball->speed == 50.0f);
    REQUIRE(ball->maxSpeed == 75.0f);
    REQUIRE(data.sound(ball->sound)->tree == "FBALL_LOOP");
    REQUIRE(data.sound(ball->sound)->soundFor('A') == "S_GRGAFBALL");
    REQUIRE(data.sound(ball->hitSound)->tree == "FBALL_EXP");
    const auto breath = data.moveNamed("BREATH");
    REQUIRE(breath.has_value());
    REQUIRE(data.moves()[*breath].type == 131);
    REQUIRE(data.damage(data.moves()[*breath].damage0)->type == AttackDefinition::kBreath);
    // A player thirty ahead: past its entrance it throws the fireball, the shot leaving the
    // head at the move's launch frame, aimed at the player's middle.
    const std::vector<EnemyView> far{playerAt(Vec3{0.0f, 0.0f, 30.0f})};
    std::vector<CombatShot> shots;
    for (s32 i = 0; i < 900 && shots.empty(); ++i) {
        critters.update(kTicks, kStep, far);
        for (const CombatShot& shot : critters.takeShots()) {
            shots.push_back(shot);
        }
    }
    REQUIRE(shots.size() == 1);
    REQUIRE(critters.moveOf(*id) == "FBALL");
    REQUIRE(shots[0].critter == *id);
    REQUIRE(shots[0].damageIndex == throwing.damage0);
    REQUIRE(shots[0].data == &data);
    REQUIRE(shots[0].target == Vec3{0.0f, 3.0f, 30.0f});
    REQUIRE(shots[0].origin.y > 2.0f); // from the head, not the feet
    // Launched, it flies as FBALL_LOOP with its sound, and strikes the player for twenty.
    ItemArchive* archive = critters.archiveOf(*id);
    REQUIRE(archive != nullptr);
    EffectTrees effects;
    CombatantProjectiles projectiles;
    std::vector<std::string> sounds;
    const CombatantProjectiles::PlaySound sound = [&](std::string_view name) {
        sounds.emplace_back(name);
    };
    projectiles.launch(shots[0], *archive, device, effects, sound);
    REQUIRE(projectiles.count() == 1);
    REQUIRE(sounds == std::vector<std::string>{"S_GRGAFBALL"});
    std::vector<CombatantProjectileHit> hits;
    for (s32 i = 0; i < 120 && hits.empty(); ++i) {
        effects.update(kStep);
        projectiles.update(kStep, &collision, far, device, effects, sound);
        for (const CombatantProjectileHit& hit : projectiles.takeHits()) {
            hits.push_back(hit);
        }
    }
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0].player == 0);
    REQUIRE(hits[0].damage == 20.0f);
    REQUIRE(hits[0].flags == ball->flags);
    REQUIRE(hits[0].direction.z > 0.0f);
    projectiles.clear(effects);
    effects.clear();
    // A player twelve ahead is breathed on instead: contacts of five, held off by the
    // player's breath gap.
    Critters near;
    near.open(device, root, &collision, EnemyScales{}, 'A');
    const auto other = near.spawn(CombatantKind::Gargoyle, Vec3{0.0f, 0.0f, 0.0f}, 0.0f);
    REQUIRE(other.has_value());
    const std::vector<EnemyView> close{playerAt(Vec3{0.0f, 0.0f, 12.0f})};
    std::vector<CombatBlow> blows;
    bool breathed = false;
    for (s32 i = 0; i < 900 && blows.empty(); ++i) {
        near.update(kTicks, kStep, close);
        breathed = breathed || near.moveOf(*other) == "BREATH";
        for (const CombatBlow& blow : near.takeBlows()) {
            blows.push_back(blow);
        }
    }
    REQUIRE(breathed);
    REQUIRE_FALSE(blows.empty());
    REQUIRE(blows[0].breath);
    REQUIRE(blows[0].damage == 5.0f);
    REQUIRE(near.takeShots().empty());
}

} // namespace
