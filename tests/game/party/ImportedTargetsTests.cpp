#include <array>
#include <limits>
#include <memory>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/level/ImportedLevel.h"
#include "game/level/ScriptedFixtures.h"
#include "game/mechanics/MechanicsFacade.h"
#include "game/party/PlayerAttacks.h"
#include "game/party/PlayerAttacksTestPeer.h"
#include "game/party/ScriptedAttacks.h"
#include "game/players/PlayerAnimator.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

class NativeTargets final : public ImportedLevel {
public:
    struct Hit {
        s32 id;
        f32 damage;
        u32 flags;
        s32 player;
    };
    std::vector<Hit> hits;
    std::vector<ImportedTarget> bodies{{0, {0, 0, 2}, 1, 3, true, false},
                                       {1, {0, 0, 5}, 1, 1, false, true}};
    void capturePresentation() override {}
    void update(f32 /*seconds*/, bool /*timeStopped*/) override {}
    void draw(RenderDevice& /*device*/, const Mat4& /*clip*/, const WorldCamera& /*camera*/,
              TreeModel::Pass /*pass*/, f32 /*presentationAlpha*/) const override {}
    std::vector<ImportedTarget> targets() const override { return bodies; }
    bool strike(s32 id, f32 power, u32 flags, s32 player) override {
        hits.push_back({id, power, flags, player});
        return id == 0;
    }
    std::vector<Obstacle> obstacles() const override {
        std::vector<Obstacle> result;
        for (const auto& target : bodies) {
            if (!target.valid()) {
                continue;
            }
            Obstacle box;
            box.centre = target.base;
            box.cylinderRadius = target.radius;
            box.height = target.height;
            box.solid = true;
            result.push_back(box);
        }
        return result;
    }
};

struct Fixture {
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
    test::AttackScript attackEvents;
    test::ScriptedAttacks attackHost{attackEvents};
    PlayerAttacks::Targets targets{opponents, fixtures, fixtureHost, attackHost};
    std::shared_ptr<NativeTargets> imported = std::make_shared<NativeTargets>();
    std::filesystem::path root;
    Fixture() {
        root = test::assetOrSkip("PDATA/WAR.WAD").parent_path().parent_path();
        WorldCollision collision;
        collision.build(
            {{{0, 1, 0}, {Vec3{-100, 0, -100}, Vec3{-100, 0, 100}, Vec3{100, 0, -100}}, 0, 4},
             {{0, 1, 0}, {Vec3{100, 0, -100}, Vec3{-100, 0, 100}, Vec3{100, 0, 100}}, 0, 4}});
        auto layout = WorldLayout::fromDecoded({}, {}, {-100, 0, -100}, {100, 30, 100});
        LevelRef ref;
        ref.realmId = -1;
        ref.realm = "Legends";
        ref.name = "native-targets";
        const bool loaded = world.loadImported(device, root, ref, std::move(layout),
                                               std::move(collision), imported);
        REQUIRE(loaded);
        arsenal.bind({device, classes, weapons, world.collision(), effects, audio, nullptr, {}});
        attacks.bind({device, classes, world, weapons, effects, audio, nullptr, arsenal, dimmer});
        fixtures.bind({device, world, weapons, effects, audio});
        players[0].actor.spawn(3, {}, nullptr, Vec3{0}, 0);
    }
};

TEST_CASE("Imported fixture IDs acquire barrels without aiming at inert chests",
          "[legends][imported-targets][assets]") {
    Fixture f;
    f.imported->bodies.push_back(
        {2, {0, std::numeric_limits<f32>::quiet_NaN(), 0}, 1, 1, true, false});
    f.imported->bodies.push_back({kImportedTargetCount, {0, 0, 0}, 1, 1, true, false});
    const auto targets = PlayerAttacksTestPeer::targets(f.attacks, f.targets);
    REQUIRE(targets.size() == 2);
    CHECK(targets[0].id == kImportedTargetBase);
    REQUIRE(targets[0].acquisition);
    CHECK(targets[0].acquisition->enabled);
    CHECK(targets[1].id == kImportedTargetBase + 1);
    REQUIRE(targets[1].acquisition);
    CHECK_FALSE(targets[1].acquisition->enabled);
    CHECK(targets[1].touches({0, .5f, 5}, 0));
    const auto selection = f.attacks.meleeAcquisition(f.players[0].actor, f.targets);
    REQUIRE(selection);
    REQUIRE(selection->targets.size() == 1);
    CHECK(selection->targets.front().low);
}

TEST_CASE("Imported melee and projectile hits cannot fall through to player or statue IDs",
          "[legends][imported-targets][assets]") {
    Fixture f;
    s32 hurtPlayers = 0;
    f.attackEvents.hurt = [&](usize, f32, HurtKind, const PlayerImpact&) { ++hurtPlayers; };
    SECTION("projectile") {
        MissileImpact impact;
        impact.target = kImportedTargetBase;
        impact.owner = 3;
        impact.damage = 17;
        impact.flags = EnemyHit::kKnockDown;
        PlayerAttacksTestPeer::projectile(f.attacks, impact, f.players, f.targets);
        REQUIRE(f.imported->hits.size() == 1);
        CHECK(f.imported->hits[0].damage == 17);
    }
    SECTION("ordinary melee contact") {
        REQUIRE(f.classes.load(f.root / "PDATA"));
        const auto* stats = f.classes.stats(0);
        REQUIRE(stats);
        f.players[0].actor.spawn(3, {}, stats, Vec3{0}, 0);
        f.players[0].figure =
            PlayerFigure::load(f.device, f.root, f.players[0].actor.save(), Entrance::Skip);
        REQUIRE(f.players[0].figure);
        f.attacks.melee(0, f.players, f.targets);
        REQUIRE(f.imported->hits.size() == 1);
        CHECK(f.imported->hits[0].damage > 0);
    }
    REQUIRE(f.imported->hits.size() == 1);
    CHECK(f.imported->hits[0].id == 0);
    CHECK(f.imported->hits[0].player == 3);
    CHECK(hurtPlayers == 0);
}

TEST_CASE("Imported fixtures share turbo and item-area dispatch",
          "[legends][imported-targets][assets]") {
    Fixture f;
    const MissileTarget target{kImportedTargetBase, {0, 0, 2}, 1, 3};
    for (const auto source : {BlowSource::Turbo, BlowSource::Breath, BlowSource::Shield}) {
        PlayerAttacksTestPeer::area(f.attacks, target, f.players, f.targets, source);
    }
    REQUIRE(f.imported->hits.size() == 3);
    for (const auto& hit : f.imported->hits) {
        CHECK(hit.id == 0);
        CHECK(hit.damage == 35);
        CHECK(hit.player == 3);
    }
}

TEST_CASE("A real player missile flight reaches imported barrels before inert stops",
          "[legends][imported-targets][assets]") {
    Fixture f;
    static constexpr MissileSpec kSpec{"TEST", {}, .1f, 0, 0, true};
    MissileLaunch shot;
    shot.spec = &kSpec;
    shot.owner = 3;
    shot.position = {0, .5f, -5};
    shot.velocity = Vec3{0, 0, 30};
    shot.damage = 18;
    REQUIRE(f.arsenal.missiles().launch(shot));
    for (s32 tick = 0; tick < 30 && f.imported->hits.empty(); ++tick) {
        f.attacks.updateProjectiles(1.0f / 60, f.players, f.targets);
    }
    REQUIRE(f.imported->hits.size() == 1);
    CHECK(f.imported->hits.front().id == 0);
    CHECK(f.imported->hits.front().damage == 18);
    CHECK(f.imported->hits.front().player == 3);
}

TEST_CASE("Imported potion waves and shields damage only eligible fixtures once per interval",
          "[legends][imported-targets][assets]") {
    Fixture f;
    SECTION("potion wave") {
        PlayerAttacksTestPeer::potion(f.attacks);
        PlayerAttacksTestPeer::potions(f.attacks, f.players, f.targets);
        PlayerAttacksTestPeer::potions(f.attacks, f.players, f.targets);
    }
    SECTION("potion shield") {
        PlayerAttacksTestPeer::shield(f.attacks);
        f.attacks.updateShields(1.0f / 60, f.players, f.targets);
        f.attacks.updateShields(1.0f / 60, f.players, f.targets);
    }
    REQUIRE(f.imported->hits.size() == 1);
    CHECK(f.imported->hits[0].id == 0);
    CHECK(f.imported->hits[0].player == 3);
    CHECK(f.imported->hits[0].damage > 0);
    CHECK((f.imported->hits[0].flags & EnemyHit::kMagic) != 0);
}

TEST_CASE("Imported ram combo and blast contacts use disjoint one-hit ledgers",
          "[legends][imported-targets][assets]") {
    Fixture f;
    SECTION("ram") {
        f.attacks.ramBarrels(0, f.players, f.targets);
        f.attacks.ramBarrels(0, f.players, f.targets);
    }
    SECTION("combo") {
        CHECK(f.attacks.comboImpact(0, 1, 25, f.players, f.targets));
        CHECK_FALSE(f.attacks.comboImpact(0, 1, 25, f.players, f.targets));
    }
    SECTION("blast") {
        std::vector<s32> reached;
        f.fixtures.blastScenery({0, 0, 0}, 10, 20, EnemyHit::kKnockDown, f.players, f.fixtureHost,
                                reached);
        f.fixtures.blastScenery({0, 0, 0}, 10, 20, EnemyHit::kKnockDown, f.players, f.fixtureHost,
                                reached);
        CHECK(reached == std::vector<s32>{kImportedTargetBase});
    }
    REQUIRE(f.imported->hits.size() == 1);
    CHECK(f.imported->hits[0].id == 0);
}

TEST_CASE("Imported closed containers block ordinary players and enemy missiles",
          "[legends][imported-targets][assets]") {
    Fixture f;
    CHECK(f.fixtures.obstacles().size() == 2);
    CHECK(f.fixtures.missileStops().size() == 2);
    CHECK(f.fixtures.critterObstacles().size() == 2);
    f.players[0].actor.place({0, 0, 2});
    f.fixtures.update(1, 1.0f / 30, f.players, f.fixtureHost);
    const Vec3 position = f.players[0].actor.position();
    CHECK(glm::length(Vec2{position.x, position.z - 2}) >= Approx(1 + f.players[0].actor.radius()));
    // Native gameplay removes these bodies immediately once the opening starts.
    f.imported->bodies.clear();
    CHECK(f.fixtures.obstacles().empty());
    CHECK(f.fixtures.missileStops().empty());
}

TEST_CASE("Imported ram and blast contacts use authored faces instead of bounding cylinders",
          "[legends][imported-targets][assets]") {
    Fixture f;
    const std::array<CollisionTriangle, 2> faces{{
        {{-1, 0, 0}, {Vec3{20, 0, -10}, Vec3{20, 8, -10}, Vec3{20, 0, 10}}, 0, 2},
        {{-1, 0, 0}, {Vec3{20, 0, 10}, Vec3{20, 8, -10}, Vec3{20, 8, 10}}, 0, 2},
    }};
    // The cylinder contains the origin, but the actual wall is twenty units away.
    // A rejected broad contact must not consume the attack's one-hit ledger.
    f.imported->bodies = {{0, {20, 0, 0}, 20, 8, true, true, faces}};
    const MissileTarget cylinder{kImportedTargetBase, {20, 0, 0}, 20, 8};
    REQUIRE(cylinder.touches({0, 3, 0}, 2));

    SECTION("ram") {
        f.attacks.ramBarrels(0, f.players, f.targets);
        CHECK(f.imported->hits.empty());
        CHECK(f.players[0].rammed.empty());

        f.players[0].actor.place({19.5f, 0, 0});
        f.attacks.ramBarrels(0, f.players, f.targets);
        f.attacks.ramBarrels(0, f.players, f.targets);
        CHECK(f.players[0].rammed == std::vector<usize>{kImportedTargetBase});
    }
    SECTION("blast") {
        std::vector<s32> reached;
        f.fixtures.blastScenery({0, 3, 0}, 2, 20, EnemyHit::kKnockDown, f.players, f.fixtureHost,
                                reached);
        CHECK(f.imported->hits.empty());
        CHECK(reached.empty());

        f.fixtures.blastScenery({19.5f, 3, 0}, 2, 20, EnemyHit::kKnockDown, f.players,
                                f.fixtureHost, reached);
        f.fixtures.blastScenery({19.5f, 3, 0}, 2, 20, EnemyHit::kKnockDown, f.players,
                                f.fixtureHost, reached);
        CHECK(reached == std::vector<s32>{kImportedTargetBase});
    }
    REQUIRE(f.imported->hits.size() == 1);
    CHECK(f.imported->hits.front().id == 0);
}
} // namespace
