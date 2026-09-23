#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/enemies/Bosses.h"
#include "game/enemies/CombatantFixture.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr s32 kTicks = 2;
constexpr f32 kStep = 1.0f / 30.0f;

EnemyView playerAt(const Vec3& position, s32 player = 0) {
    EnemyView view;
    view.player = player;
    view.position = position;
    view.radius = 1.0f;
    view.height = 6.0f;
    return view;
}

TEST_CASE("Wraith waits in its lowered entrance pose and rises only in START3",
          "[game][enemies][wraith][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/WRAITH.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/WRAITH/animations.json");
    test::FakeRenderDevice device;
    Bosses bosses;
    bosses.open(device, root, nullptr, {}, 'J');
    REQUIRE(bosses.spawn(40, Vec3{0}, 0));
    REQUIRE(bosses.moveName() == "START");
    const auto initial = bosses.nodeTransform("ROOT");
    REQUIRE(initial.has_value());
    // The negative key cancels ROOT's 24.9408-unit bind offset above L1ROOT.
    REQUIRE((*initial)[3].y == Approx(4.020784f));
    const std::vector<EnemyView> far{playerAt({0, 0, 100})};
    bosses.update(kTicks, kStep, far);
    REQUIRE_FALSE(bosses.view().awake);
    REQUIRE(bosses.nodeTransform("ROOT") == initial);

    const std::vector<EnemyView> near{playerAt({0, 0, 40})};
    bool second = false;
    bool third = false;
    bool risen = false;
    for (s32 frame = 0; frame < 450 && !risen; ++frame) {
        bosses.update(kTicks, kStep, near);
        const auto pose = bosses.nodeTransform("ROOT");
        REQUIRE(pose.has_value());
        if (bosses.moveName() == "START" || bosses.moveName() == "START2") {
            REQUIRE((*pose)[3].y == Approx((*initial)[3].y));
            second |= bosses.moveName() == "START2";
        } else if (bosses.moveName() == "START3") {
            third = true;
            risen |= (*pose)[3].y > (*initial)[3].y + 10.0f;
        }
        bosses.takeCues();
    }
    REQUIRE(second);
    REQUIRE(third);
    REQUIRE(risen);
}

TEST_CASE("Wraith health and range windows expose every authored attack family",
          "[game][enemies][wraith][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/WRAITH.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/WRAITH/animations.json");
    test::FakeRenderDevice device;
    test::CombatantFixture fixture;
    fixture.open(device, root, nullptr, {}, 'J');
    REQUIRE(fixture.spawn("WRAITH", {0, -15.9375f, -35}, 0));
    std::set<std::string> attacks;
    std::set<s32> projectiles;
    bool rootLaunch = false;
    for (const f32 health : {1.0f, 0.7f, 0.4f, 0.15f}) {
        for (const Vec3 offset :
             {Vec3{0, 0, 15}, Vec3{0, 0, 45}, Vec3{0, 0, 70}, Vec3{35, 0, 35}, Vec3{-35, 0, 35}}) {
            REQUIRE(
                fixture.actor.spawn(fixture.assets, 0, {0, -15.9375f, -35}, 0, nullptr, {}, 'J'));
            if (health < 1) {
                EnemyHit hit;
                hit.damage = fixture.actor.maxHealth() * (1 - health);
                fixture.actor.hurt(hit);
            }
            const std::vector<EnemyView> players{playerAt(Vec3{0, 0, -35} + offset)};
            for (s32 frame = 0; frame < 7200; ++frame) {
                fixture.update(2, kStep, players);
                if (fixture.actor.moveType() >= 128) {
                    attacks.emplace(fixture.actor.moveName());
                }
                for (const auto& shot : fixture.actor.takeShots()) {
                    projectiles.insert(shot.damageIndex);
                    if (shot.damageIndex == 10) {
                        REQUIRE(shot.origin.y == Approx(1.0625f));
                        rootLaunch = true;
                    }
                }
                fixture.actor.takeCues();
                fixture.actor.takeBlows();
            }
            REQUIRE(glm::length(Vec2{fixture.actor.position().x,
                                     fixture.actor.position().z + 35}) <= 3.001f);
        }
    }
    REQUIRE(attacks == std::set<std::string>{"SWIPEL", "SWIPER", "BLENDER", "SPINBLADE", "RSTRETCH",
                                             "LSTRETCH", "THROW", "PECKERSHOT", "SNAKES", "GBALL",
                                             "BOLT"});
    REQUIRE(projectiles == std::set<s32>{8, 10, 11, 12});
    REQUIRE(rootLaunch);
}

TEST_CASE("the bosses are named by kind, from the dragon to the garm", "[game][enemies]") {
    REQUIRE(bossNameOf(41) == "LICH");
    REQUIRE(bossNameOf(34) == "DRAGON");
    REQUIRE(bossNameOf(44) == "GARM");
    REQUIRE(bossNameOf(-1).empty());
    REQUIRE(bossNameOf(33).empty());
    BossView none;
    REQUIRE(none.fraction() == 0.0f);
    none.health = 750.0f;
    none.maxHealth = 3000.0f;
    REQUIRE(none.fraction() == Approx(0.25f));
}

TEST_CASE("an absent boss has no height or camera offset", "[game][enemies]") {
    Bosses bosses;
    REQUIRE(bosses.height() == 0.0f);
    REQUIRE(bosses.cameraOffset() == Vec3{0.0f});
    bosses.close();
    REQUIRE(bosses.height() == 0.0f);
    REQUIRE(bosses.cameraOffset() == Vec3{0.0f});
}

TEST_CASE("arena mesh cues belong only to the Lich and Garm", "[game][enemies][boss-effects]") {
    REQUIRE(bossArenaObject(41) == "G5BIGDIRT");
    REQUIRE(bossArenaObject(44) == "H4NSFFXL_PURPLE");
    for (s32 kind = -1; kind <= 45; ++kind) {
        if (kind != 41 && kind != 44) {
            REQUIRE(bossArenaObject(kind).empty());
        }
    }
}

TEST_CASE("a boss sleeps until the party comes near, then fights by its table, and its "
          "meter follows its health to the end",
          "[game][enemies][unpacked]") {
    const std::filesystem::path root =
        test::unpackedOrSkip("critter/LICH.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/LICH/animations.json");
    test::FakeRenderDevice device;
    Bosses bosses;
    EnemyScales scales;
    scales.health = 0.5f;
    bosses.open(device, root, nullptr, scales, 'G');
    REQUIRE_FALSE(bosses.present());
    REQUIRE_FALSE(bosses.spawn(33, Vec3{0.0f, 0.0f, 0.0f}, 0.0f)); // no boss of that kind
    REQUIRE(bosses.spawn(41, Vec3{0.0f, 0.0f, 0.0f}, 0.0f, 20.0f));
    REQUIRE_FALSE(bosses.spawn(41, Vec3{0.0f, 0.0f, 0.0f}, 0.0f)); // one to a level
    REQUIRE(bosses.present());
    BossView view = bosses.view();
    REQUIRE(view.kind == 41);
    REQUIRE(view.name == "LICH");
    REQUIRE(view.maxHealth == 1500.0f); // three thousand at the level's half
    REQUIRE(view.health == 1500.0f);
    REQUIRE(view.fraction() == 1.0f);
    REQUIRE(view.alive);
    REQUIRE_FALSE(view.awake);
    REQUIRE(bosses.targets().size() == 1);
    // Its meter is two strips with backgrounds, capped 44 on the left and 53 on the right,
    // drawn from its own archive.
    const HealthMeterDefinition* meter = bosses.meter();
    REQUIRE(meter != nullptr);
    REQUIRE(meter->shown);
    REQUIRE(meter->backed);
    REQUIRE(meter->pieces == 2);
    REQUIRE(meter->advance == 256);
    REQUIRE(meter->leftInset == 44);
    REQUIRE(meter->rightInset == 53);
    REQUIRE(meter->barOffset == Vec3{-4.0f, 4.0f, 0.0f});
    REQUIRE(bosses.archive() != nullptr);
    REQUIRE(bosses.archive()->textures.find("METER_FG1").has_value());
    // Beyond its threshold it sleeps: nothing moves.
    const std::vector<EnemyView> far{playerAt(Vec3{0.0f, 0.0f, 40.0f})};
    for (s32 i = 0; i < 120; ++i) {
        bosses.update(kTicks, kStep, far);
    }
    REQUIRE_FALSE(bosses.view().awake);
    REQUIRE(bosses.moveName() == "START");
    REQUIRE(bosses.position()->z == 0.0f);
    // Within it, it wakes, makes its entrance and comes for the player.
    const std::vector<EnemyView> near{playerAt(Vec3{0.0f, 0.0f, 18.0f})};
    bosses.update(kTicks, kStep, near);
    REQUIRE(bosses.view().awake);
    std::vector<CombatBlow> blows;
    for (s32 i = 0; i < 1500 && blows.empty(); ++i) {
        bosses.update(kTicks, kStep, near);
        auto taken = bosses.takeBlows();
        blows.insert(blows.end(), taken.begin(), taken.end());
    }
    REQUIRE_FALSE(blows.empty());
    REQUIRE(blows[0].player == 0);
    REQUIRE(bosses.position()->z > 5.0f);
    // Sounds and visuals start together at sfxFrame; the glow's own sequence contains
    // the wind-up. Root-attached effects follow the full transform, not just translation.
    const std::vector<CombatCue> cues = bosses.takeCues();
    bool entrance = false;
    bool swingSound = false;
    bool swingGlow = false;
    for (const CombatCue& cue : cues) {
        entrance = entrance || (cue.tree == "GENFX" && cue.sound == "S_LICHENT");
        if (cue.sound.starts_with("S_LICHATK")) {
            swingSound = true;
        }
        if (cue.tree.starts_with("ATK")) {
            swingGlow = true;
            REQUIRE(cue.follows);
            REQUIRE(cue.rootAttachment);
        }
    }
    REQUIRE(entrance);
    REQUIRE(swingSound);
    REQUIRE(swingGlow);
    // Struck, the meter falls; the blow pays its share; killed, a fifth of its value goes to
    // everyone, and once its death has played it is gone.
    EnemyHit hit;
    hit.damage = 300.0f;
    hit.player = 0;
    bosses.hurt(hit);
    view = bosses.view();
    REQUIRE(view.health == Approx(1500.0f - 300.0f + 1.0f)); // its armour of one
    REQUIRE(view.fraction() == Approx(view.health / 1500.0f));
    auto losses = bosses.takeLosses();
    REQUIRE(losses.size() == 1);
    REQUIRE(losses[0].player == 0);
    REQUIRE(losses[0].experience > 0.0f);
    EnemyHit slay;
    slay.damage = 5000.0f;
    slay.player = 0;
    bosses.hurt(slay);
    losses = bosses.takeLosses();
    REQUIRE(losses.size() == 2);
    REQUIRE(losses[1].killed);
    REQUIRE(losses[1].player == -1);
    REQUIRE(losses[1].experience == Approx(0.2f * 3860.0f));
    REQUIRE_FALSE(bosses.view().alive);
    REQUIRE(bosses.view().fraction() == 0.0f);
    s32 gone = 0;
    while (bosses.present() && gone < 900) {
        bosses.update(kTicks, kStep, near);
        ++gone;
    }
    REQUIRE_FALSE(bosses.present());
    REQUIRE(gone > 10);
}

TEST_CASE("a legend item brought to the boss is thrown as it rises and takes its toll",
          "[game][enemies][unpacked]") {
    const std::filesystem::path root =
        test::unpackedOrSkip("critter/LICH.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/LICH/animations.json");
    test::FakeRenderDevice device;
    Bosses bosses;
    EnemyScales scales;
    scales.health = 0.5f;
    bosses.open(device, root, nullptr, scales, 'G');
    REQUIRE_FALSE(bosses.bringLegend(0)); // no boss yet
    REQUIRE(bosses.spawn(41, Vec3{0.0f, 0.0f, 0.0f}, 0.0f, 20.0f));
    // The lich's item is the town's, the book of protection.
    REQUIRE(bosses.legendRealm() == 7);
    REQUIRE(bosses.bringLegend(0));
    REQUIRE_FALSE(bosses.bringLegend(1)); // one rite
    REQUIRE(bosses.legend().stage() == LegendRite::Stage::Carried);
    // Asleep, nothing happens.
    const std::vector<EnemyView> far{playerAt(Vec3{0.0f, 0.0f, 40.0f})};
    for (s32 i = 0; i < 60; ++i) {
        bosses.update(kTicks, kStep, far);
    }
    REQUIRE(bosses.takeLegendEvents().empty());
    REQUIRE(bosses.legend().stage() == LegendRite::Stage::Carried);
    // Awake, it rises, and once risen the item goes up and is thrown: a quarter of its
    // health goes at once, less its armour of one, paid as a hit to the bearer.
    const std::vector<EnemyView> near{playerAt(Vec3{0.0f, 0.0f, 18.0f})};
    std::vector<LegendEvent> events;
    s32 waited = 0;
    while (!bosses.legend().thrown() && waited < 3000) {
        bosses.update(kTicks, kStep, near);
        auto taken = bosses.takeLegendEvents();
        events.insert(events.end(), taken.begin(), taken.end());
        ++waited;
    }
    REQUIRE(bosses.legend().thrown());
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].cue == LegendCue::Brandished);
    REQUIRE(events[0].player == 0);
    REQUIRE(events[0].realm == 7);
    REQUIRE(events[1].cue == LegendCue::Thrown);
    REQUIRE(bosses.view().health == Approx(1500.0f - (0.25f * 1500.0f - 1.0f)));
    auto losses = bosses.takeLosses();
    REQUIRE(losses.size() == 1);
    REQUIRE(losses[0].player == 0);
    REQUIRE_FALSE(bosses.frozen());
    REQUIRE_FALSE(bosses.curbed());
    // It roars at that, and the rite is over: the book's toll is paid once.
    bool roared = false;
    for (s32 i = 0; i < 600 && !roared; ++i) {
        bosses.update(kTicks, kStep, near);
        for (const LegendEvent& event : bosses.takeLegendEvents()) {
            roared = roared || event.cue == LegendCue::Roared;
        }
    }
    REQUIRE(roared);
    REQUIRE(bosses.legend().stage() == LegendRite::Stage::Over);
    REQUIRE_FALSE(bosses.legend().running());
    // Then it fights.
    std::vector<CombatBlow> blows;
    for (s32 i = 0; i < 1500 && blows.empty(); ++i) {
        bosses.update(kTicks, kStep, near);
        auto taken = bosses.takeBlows();
        blows.insert(blows.end(), taken.begin(), taken.end());
    }
    REQUIRE_FALSE(blows.empty());
    bosses.close();
    REQUIRE(bosses.legend().stage() == LegendRite::Stage::None);
}

TEST_CASE("the genie selects projectile attacks and launches them from its animated body",
          "[game][boss-projectiles][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/DJINN.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/DJINN/animations.json");
    test::FakeRenderDevice device;
    Bosses bosses;
    bosses.open(device, root, nullptr, {}, 'C');
    REQUIRE(bosses.spawn(36, Vec3{0}, 0, 100));
    REQUIRE(bosses.cameraBase() == Vec3{0, 7, 0});
    REQUIRE(bosses.cameraOffset() == Vec3{0, 17, 0});
    const std::vector<EnemyView> party{playerAt(Vec3{0, 0, 40})};
    std::vector<CombatShot> shots;
    for (s32 frame = 0; frame < 900 && shots.empty(); ++frame) {
        bosses.update(kTicks, kStep, party);
        shots = bosses.takeShots();
    }
    REQUIRE_FALSE(shots.empty());
    for (const CombatShot& shot : shots) {
        REQUIRE(shot.data != nullptr);
        REQUIRE(shot.data->name() == "DJINN");
        REQUIRE(shot.data->damage(shot.damageIndex)->type == AttackDefinition::kProjectile);
        REQUIRE(shot.realm == 'C');
        REQUIRE(shot.target == Vec3{0, 3, 40});
        REQUIRE(shot.origin.y > 0);
    }
    bosses.close();
    REQUIRE(bosses.takeShots().empty());
    REQUIRE_FALSE(bosses.cameraBase().has_value());
}

TEST_CASE("anchored bosses retain local territories and the lich and spider can pursue",
          "[game][boss-movement][unpacked]") {
    struct Expected {
        const char* name;
        f32 radius;
        bool pursuit;
    };
    for (const auto& expected :
         {Expected{"DRAGON", 3, false}, Expected{"CHIMERA", 12, false}, Expected{"DJINN", 0, false},
          Expected{"PBOSS", 6, false}, Expected{"YETI", 5, false}, Expected{"WRAITH", 3, false},
          Expected{"LICH", 25, true}, Expected{"DRIDER", 22, true}}) {
        DYNAMIC_SECTION(expected.name) {
            const auto path =
                test::unpackedOrSkip(std::string("critter/") + expected.name + ".json");
            CritterData data;
            REQUIRE(data.load(path));
            REQUIRE(data.movement().roamRadius == expected.radius);
            bool pursues = false;
            bool hasNearAttack = false;
            bool hasRangedAttack = false;
            for (const MoveDefinition& move : data.moves()) {
                if (move.type == MoveDefinition::kWalk || move.type == 134) {
                    pursues = pursues || move.speed > 0;
                }
                if (move.attack() && move.harms()) {
                    hasNearAttack = hasNearAttack ||
                                    (move.target.maxDistance > 0 && move.target.maxDistance <= 40);
                    const AttackDefinition* harm = data.damage(move.damage0);
                    hasRangedAttack =
                        hasRangedAttack ||
                        (harm != nullptr && harm->type == AttackDefinition::kProjectile);
                }
            }
            REQUIRE(pursues == expected.pursuit);
            // Chimera's separate head move tables are not its root body's attacks.
            if (std::string_view(expected.name) != "CHIMERA") {
                REQUIRE(hasNearAttack);
                REQUIRE(hasRangedAttack);
            }
        }
    }
}

TEST_CASE("every retail boss can enter animate draw take damage and die",
          "[game][boss-roster][unpacked]") {
    for (s32 kind = 34; kind <= 44; ++kind) {
        const std::string name{bossNameOf(kind)};
        DYNAMIC_SECTION(name) {
            const auto root =
                test::unpackedOrSkip("critter/" + name + ".json").parent_path().parent_path();
            test::unpackedOrSkip("MONSTERS/" + name + "/animations.json");
            test::FakeRenderDevice device;
            Bosses bosses;
            bosses.open(device, root, nullptr, {}, 'G');
            REQUIRE(bosses.spawn(kind, Vec3{0}, 0, 100));
            REQUIRE(bosses.view().alive);
            REQUIRE(bosses.view().maxHealth > 0);
            const std::vector<EnemyView> party{playerAt(Vec3{0, 0, 20})};
            for (s32 tick = 0; tick < 180; ++tick) {
                bosses.update(kTicks, kStep, party);
            }
            REQUIRE(bosses.view().awake);
            REQUIRE_FALSE(bosses.moveName().empty());
            bosses.draw(device, Mat4{1}, {});
            REQUIRE_FALSE(device.draws.empty());
            EnemyHit hit;
            hit.player = 0;
            hit.damage = bosses.view().maxHealth * 2;
            bosses.hurt(hit);
            REQUIRE_FALSE(bosses.view().alive);
            REQUIRE_FALSE(bosses.takeLosses().empty());
        }
    }
}

} // namespace
