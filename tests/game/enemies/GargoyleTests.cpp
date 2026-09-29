#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/world/WorldCollision.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
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
