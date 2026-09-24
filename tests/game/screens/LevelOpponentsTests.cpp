#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/LevelFixtures.h"
#include "game/screens/LevelOpponents.h"
#include "game/world/SafeRocks.h"
namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Forsaken Province entrance generators breed with the placed enemy roster loaded",
          "[level-opponents][generators][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELG1/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/ZOM/animations.json");
    test::unpackedOrSkip("MONSTERS/MAG/animations.json");
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    LevelWorld world;
    REQUIRE(world.load(device, root, *catalog.byName("G1")));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelFixtures fixtures;
    fixtures.bind({device, world, weapons, effects, audio, 1});
    fixtures.setPlayerCount(1);
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, {24.375f, 0.0078125f, 2.5f}, 0);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    REQUIRE(opponents.enemies().count() == static_cast<usize>(world.level()->maxEnemies));
    std::array<bool, 3> special{};
    bool skirmishBomber = false;
    for (s32 id = 0; id < Enemies::kMost; ++id) {
        if (!opponents.enemies().alive(id)) {
            continue;
        }
        const s32 variant = opponents.enemies().variantOf(id);
        if (variant >= kArcherStrength && variant <= kSuicideStrength) {
            special[static_cast<usize>(variant - kArcherStrength)] = true;
        }
        skirmishBomber = skirmishBomber || opponents.enemies().algorithmOf(id) == kSkirmishBombWay;
    }
    CHECK(special == std::array<bool, 3>{true, true, true});
    CHECK(skirmishBomber);
    LevelOpponents::Events events;
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    events.blast = [](const Vec3&, f32, f32) {};
    events.settleBlasts = [] {};
    events.legend = [](const LegendEvent&) {};
    events.advanceLegend = [](f32) {};
    events.fallen = [](const Vec3&) {};
    events.spew = [](const CombatSpew&) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    for (s32 frame = 0; frame < 300; ++frame) {
        opponents.update(2, 1.0f / 30, players, fixtures.obstacles(), events);
    }
    s32 bred = 0;
    s32 weakBred = 0;
    for (usize g = 0; g < opponents.generators().count(); ++g) {
        const auto id = static_cast<s32>(g);
        bred += opponents.generators().bredOf(id);
        if (opponents.generators().tierOf(id) == 1) {
            weakBred += opponents.generators().bredOf(id);
        }
    }
    CHECK(bred > 0);
    CHECK(weakBred > 0);
    CHECK(opponents.enemies().count() <= static_cast<usize>(world.level()->maxEnemies));
    opponents.close();
    fixtures.clear();
    effects.clear();
}

TEST_CASE("exit settlement credits a last-frame generator kill once without advancing combat",
          "[shop][level-opponents][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELG1/world.json").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level.has_value());
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(3, {}, nullptr, {0, 0, 0}, 0);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    s32 generator = -1;
    for (usize i = 0; i < opponents.generators().count(); ++i) {
        if (opponents.generators().standing(static_cast<s32>(i))) {
            generator = static_cast<s32>(i);
            break;
        }
    }
    REQUIRE(generator >= 0);
    s32 credited = 0;
    LevelOpponents::Events events;
    events.award = [&](s32 player, s32 amount, bool killed) {
        REQUIRE(player == 3);
        REQUIRE(amount == 0);
        REQUIRE(killed);
        ++credited;
    };
    events.levels = [] {};
    opponents.strikeGenerator(generator, 1000000, 3);
    REQUIRE_FALSE(opponents.generators().standing(generator));
    opponents.settleRewards(players, events);
    REQUIRE(credited == 1);
    opponents.strikeGenerator(generator, 1000000, 3);
    opponents.settleRewards(players, events);
    REQUIRE(credited == 1);
    opponents.close();
    effects.clear();
}

TEST_CASE("Wraith entrance stops its persistent portal before the emergence effects",
          "[game][screens][level-opponents][wraith][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/WRAITH.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/WRAITH/animations.json");
    test::unpackedOrSkip("LEVELS/LEVELJ5/world.json");
    test::unpackedOrSkip("ITEMS/LEVELJ5/objects.json");
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("J5");
    REQUIRE(level.has_value());
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, {0, 0, 0}, 0);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    REQUIRE(opponents.bosses().view().kind == 40);
    LevelOpponents::Events events;
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    events.blast = [](const Vec3&, f32, f32) {};
    events.settleBlasts = [] {};
    events.legend = [](const LegendEvent&) {};
    events.advanceLegend = [](f32) {};
    events.fallen = [](const Vec3&) {};
    events.spew = [](const CombatSpew&) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    u32 portal = 0;
    bool emerged = false;
    bool second = false;
    for (s32 frame = 0; frame < 900 && !second; ++frame) {
        opponents.update(2, 1.0f / 30, players, {}, events);
        for (usize i = 0; i < effects.count(); ++i) {
            const auto& effect = effects.effect(i);
            if (effect.name == "INITFX") {
                REQUIRE_FALSE(emerged);
                portal = effect.id;
                REQUIRE(effect.secondsLeft > 1000);
            } else if (effect.name == "GENFX") {
                REQUIRE(portal != 0);
                REQUIRE_FALSE(effects.playing(portal));
                emerged = true;
            } else if (effect.name == "GENFX2") {
                second = true;
            }
        }
        effects.update(1.0f / 30);
    }
    REQUIRE(portal != 0);
    REQUIRE(emerged);
    REQUIRE(second);
    opponents.close();
    REQUIRE(effects.count() == 0);
}

TEST_CASE("Yeti POUND places a single I5 eruption and restores that arena obstacle",
          "[game][screens][level-opponents][yeti][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/YETI.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/YETI/animations.json");
    test::unpackedOrSkip("LEVELS/LEVELI5/world.json");
    test::unpackedOrSkip("ITEMS/LEVELI5/objects.json");
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("I5");
    REQUIRE(level.has_value());
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    SafeRocks rocks;
    REQUIRE(rocks.bind(device, world.layout(), world.items()));
    rocks.setPlayerCount(1);
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, rocks.rock(3).position, 0);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    REQUIRE(opponents.bosses().view().kind == 39);
    REQUIRE(opponents.bosses().raisesArenaRocks());
    rocks.hideForEruptions();
    LevelOpponents::Events events;
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    events.blast = [](const Vec3&, f32, f32) {};
    events.settleBlasts = [] {};
    events.legend = [](const LegendEvent&) {};
    events.advanceLegend = [](f32) {};
    events.fallen = [](const Vec3&) {};
    events.spew = [](const CombatSpew&) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    events.arenaTargets = [&rocks] { return rocks.arenaTargets(); };
    usize eruptions = 0;
    events.activateArena = [&](const CombatArenaActivation& activation) {
        REQUIRE(activation.index == 3);
        rocks.scheduleActivation(activation.index, activation.delay);
        ++eruptions;
    };
    for (s32 frame = 0; frame < 3600 && eruptions == 0; ++frame) {
        rocks.update(1.0f / 30.0f);
        opponents.update(2, 1.0f / 30.0f, players, rocks.obstacles(), events);
        if (eruptions == 0) {
            effects.update(1.0f / 30.0f);
        }
    }
    INFO("Last move: " << opponents.bosses().moveName());
    REQUIRE(eruptions == 1);
    usize visuals = 0;
    for (usize i = 0; i < effects.count(); ++i) {
        const auto& effect = effects.effect(i);
        if (effect.name == "ATTACK12_S0") {
            REQUIRE(effect.attachment.has_value());
            REQUIRE(glm::length(effect.position - rocks.rock(3).position) < 0.001f);
            ++visuals;
        }
    }
    REQUIRE(visuals == 1);
    REQUIRE(rocks.obstacles().empty());
    rocks.update(34.0f / 30.0f);
    REQUIRE_FALSE(rocks.standing(3));
    rocks.update(1.01f / 30.0f);
    REQUIRE(rocks.standing(3));
    REQUIRE(rocks.obstacles().size() == 1);
    REQUIRE(rocks.rock(3).health == 90);
    opponents.close();
    for (usize i = 0; i < effects.count(); ++i) {
        INFO("Surviving effect: " << effects.effect(i).name);
        REQUIRE(effects.count() == 0); // effects cannot retain a freed boss archive
    }
}

TEST_CASE("Plague Fiend eruptions are rendered at all three K5 arena anchors",
          "[game][screens][level-opponents][plague][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/PBOSS.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/PBOSS/animations.json");
    test::unpackedOrSkip("LEVELS/LEVELK5/world.json");
    test::unpackedOrSkip("ITEMS/LEVELK5/objects.json");
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("K5");
    REQUIRE(level.has_value());
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    SafeRocks rocks;
    REQUIRE(rocks.bind(device, world.layout(), world.items()));
    rocks.setPlayerCount(1);
    const auto anchors = rocks.attackAnchors();
    REQUIRE(anchors.size() == 3);
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0, 0, 35}, 0);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    REQUIRE(opponents.bosses().view().kind == 38);
    LevelOpponents::Events events;
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    events.blast = [](const Vec3&, f32, f32) {};
    events.settleBlasts = [] {};
    events.legend = [](const LegendEvent&) {};
    events.advanceLegend = [](f32) {};
    events.fallen = [](const Vec3&) {};
    events.spew = [](const CombatSpew&) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    events.arenaAnchors = [&rocks] { return rocks.attackAnchors(); };
    usize eruptions = 0;
    for (s32 frame = 0; frame < 3600 && eruptions == 0; ++frame) {
        opponents.update(2, 1.0f / 30.0f, players, {}, events);
        for (usize i = 0; i < effects.count(); ++i) {
            const auto& effect = effects.effect(i);
            if (effect.name == "ATCK10FX") {
                REQUIRE(effect.attachment.has_value());
                REQUIRE(std::ranges::any_of(anchors, [&](const Mat4& anchor) {
                    return glm::length(Vec3{anchor[3]} - effect.position) < 0.001f &&
                           glm::length(Vec3{anchor[2]} - Vec3{effect.transform()[2]}) < 0.001f;
                }));
                ++eruptions;
            }
        }
        effects.update(1.0f / 30.0f);
    }
    INFO("Last move: " << opponents.bosses().moveName());
    REQUIRE(eruptions == 3);
    opponents.close();
    REQUIRE(effects.count() == 0);
}

TEST_CASE("opponent views preserve player identity and hide fallen participants",
          "[game][screens][level-opponents]") {
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(3, {}, nullptr, Vec3{10, 0, 20}, 0);
    players[1].actor.spawn(1, {}, nullptr, Vec3{30, 0, 40}, 0);
    players[1].life = PlayerLife::Dying;
    players[0].actor.save().progress().inventory.addPowerup(9, 4, 0, 30);
    const auto views = LevelOpponents::enemyViews(players);
    REQUIRE(views.size() == 2);
    REQUIRE(views[0].player == 3);
    REQUIRE(views[0].position == players[0].actor.position());
    REQUIRE(views[0].radius == players[0].actor.radius());
    REQUIRE_FALSE(views[0].hidden);
    REQUIRE(views[0].invisible);
    REQUIRE(views[1].player == 1);
    REQUIRE(views[1].hidden);
}

TEST_CASE("opponent phases interleave legend victory and progression in order",
          "[game][screens][level-opponents]") {
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    const auto root = test::scratchDirectory("level-opponents-empty");
    std::vector<std::string> phases;
    const LevelOpponents::Events events{
        .hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) { FAIL("No combatants"); },
        .blast = [](const Vec3&, f32, f32) { FAIL("No combatants"); },
        .settleBlasts = [&] { phases.emplace_back("blast"); },
        .legend = [](const LegendEvent&) { FAIL("No boss"); },
        .advanceLegend =
            [&](f32 seconds) {
                REQUIRE(seconds == 0.1f);
                phases.emplace_back("legend");
            },
        .fallen = [](const Vec3&) { FAIL("No boss"); },
        .spew = [](const CombatSpew&) { FAIL("No boss"); },
        .advanceVictory =
            [&](s32 ticks, f32 seconds) {
                REQUIRE(ticks == 6);
                REQUIRE(seconds == 0.1f);
                phases.emplace_back("victory");
            },
        .levels = [&] { phases.emplace_back("levels"); },
        .award = [](s32, s32, bool) { FAIL("No kills"); },
        .blocksBreath = {},
        .blocksArea = {},
        .arenaAnchors = {},
        .arenaTargets = {},
        .activateArena = {},
        .shake = {}};
    opponents.update(6, 0.1f, {}, {}, events);
    REQUIRE(phases.empty());
    opponents.open({device, world, weapons, effects, audio, root, 1}, {});
    opponents.update(6, 0.1f, {}, {}, events);
    REQUIRE(phases == std::vector<std::string>{"blast", "legend", "victory", "levels"});
    opponents.close();
    opponents.close();
    opponents.update(6, 0.1f, {}, {}, events);
    REQUIRE(phases.size() == 4);
    REQUIRE_FALSE(opponents.bosses().present());
    REQUIRE_FALSE(opponents.meter().bound());
}

TEST_CASE("area immunity counts down independently for each player's runtime",
          "[game][screens][level-opponents][boss-areas]") {
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    const auto root = test::scratchDirectory("area-immunity");
    std::array<PlayerRuntime, 2> players;
    players[0].actor.spawn(3, {}, nullptr, {}, 0);
    players[1].actor.spawn(1, {}, nullptr, {}, 0);
    players[0].effectGap = 0.25f;
    players[1].effectGap = 0.05f;
    LevelOpponents::Events events;
    events.settleBlasts = [] {};
    events.advanceLegend = [](f32) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    opponents.update(6, 0.1f, players, {}, events);
    REQUIRE(players[0].effectGap > 0.14f);
    REQUIRE(players[0].effectGap < 0.16f);
    REQUIRE(players[1].effectGap == 0);
    opponents.update(12, 0.2f, players, {}, events);
    REQUIRE(players[0].effectGap == 0);
}

TEST_CASE("area contacts share effect immunity but not the breath timer",
          "[game][screens][level-opponents][boss-areas]") {
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(3, {}, nullptr, Vec3{0, 0, 12}, 0);
    LevelOpponents::Events events;
    s32 contacts = 0;
    events.hurt = [&](usize index, f32 amount, HurtKind kind, bool directed, const PlayerImpact&) {
        REQUIRE(index == 0);
        REQUIRE(amount == 50);
        REQUIRE(kind == HurtKind::Blow);
        REQUIRE(directed);
        ++contacts;
    };
    CombatBlow blow;
    blow.player = 3;
    blow.damage = 50;
    blow.area = true;
    blow.repeatGap = 0.25f;
    bool blocked = true;
    events.blocksArea = [&](const Vec3&, const Vec3&) { return blocked; };
    LevelOpponents::applyCritterBlow(blow, players, events);
    REQUIRE(contacts == 0);
    REQUIRE(players[0].effectGap == 0);
    blocked = false;
    LevelOpponents::applyCritterBlow(blow, players, events);
    REQUIRE(contacts == 1);
    REQUIRE(players[0].effectGap == 0.25f);
    REQUIRE(players[0].breathGap == 0);
    blow.critter = 7;
    LevelOpponents::applyCritterBlow(blow, players, events);
    REQUIRE(contacts == 1);
    players[0].effectGap = 0;
    players[0].breathGap = 1;
    blocked = true;
    blow.origin.z = 3; // contacts within ten units do not consult cover
    LevelOpponents::applyCritterBlow(blow, players, events);
    REQUIRE(contacts == 2);
    players[0].effectGap = 0;
    players[0].life = PlayerLife::Dying;
    LevelOpponents::applyCritterBlow(blow, players, events);
    REQUIRE(contacts == 2);
}

TEST_CASE("breath contacts share a player's quarter-second gate across creatures",
          "[game][screens][level-opponents][breath]") {
    std::array<PlayerRuntime, 3> players;
    players[0].actor.spawn(3, {}, nullptr, {}, 0);
    players[1].actor.spawn(1, {}, nullptr, {}, 0);
    players[2].actor.spawn(2, {}, nullptr, {}, 0);
    players[2].life = PlayerLife::Dying;
    std::vector<usize> hurt;
    std::vector<HurtKind> kinds;
    LevelOpponents::Events events;
    events.hurt = [&](usize index, f32 damage, HurtKind kind, bool directed,
                      const PlayerImpact& impact) {
        REQUIRE(damage == 40);
        REQUIRE(directed);
        REQUIRE(impact.flags == PlayerImpact::kKnockDown);
        REQUIRE(impact.direction == Vec3(0, 0, -1));
        hurt.push_back(index);
        kinds.push_back(kind);
    };
    CombatBlow fire;
    fire.player = 3;
    fire.damage = 40;
    fire.flags = PlayerImpact::kKnockDown;
    fire.direction = {0, 0, -1};
    fire.breath = true;
    LevelOpponents::applyCritterBlow(fire, players, events);
    REQUIRE(hurt == std::vector<usize>{0});
    REQUIRE(kinds.back() == HurtKind::Burn);
    REQUIRE(players[0].breathGap == 0.25f);
    fire.critter = 7;
    LevelOpponents::applyCritterBlow(fire, players, events);
    REQUIRE(hurt.size() == 1);
    fire.player = 1;
    LevelOpponents::applyCritterBlow(fire, players, events);
    REQUIRE(hurt == std::vector<usize>{0, 1});
    fire.player = 2;
    LevelOpponents::applyCritterBlow(fire, players, events);
    REQUIRE(hurt.size() == 2);
    fire.player = 3;
    fire.breath = false;
    LevelOpponents::applyCritterBlow(fire, players, events);
    REQUIRE(hurt.size() == 3);
    REQUIRE(kinds.back() == HurtKind::Blow);
    REQUIRE(players[0].breathGap == 0.25f);

    // Run the real level phase to expire the gate, without any assets/combatants.
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    opponents.open(
        {device, world, weapons, effects, audio, test::scratchDirectory("breath-contact-level"), 1},
        {});
    events.settleBlasts = [] {};
    events.advanceLegend = [](f32) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    opponents.update(14, 0.24f, players, {}, events);
    fire.breath = true;
    LevelOpponents::applyCritterBlow(fire, players, events);
    REQUIRE(hurt.size() == 3);
    opponents.update(1, 0.011f, players, {}, events);
    LevelOpponents::applyCritterBlow(fire, players, events);
    REQUIRE(hurt.size() == 4);
    REQUIRE(players[0].breathGap == 0.25f);
}

TEST_CASE("breath blocked by arena cover neither damages nor consumes the breath timer",
          "[game][screens][level-opponents][breath]") {
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0, 0, 20}, 0);
    LevelOpponents::Events events;
    s32 hits = 0;
    events.hurt = [&](usize, f32, HurtKind, bool, const PlayerImpact&) { ++hits; };
    bool blocked = true;
    events.blocksBreath = [&](const Vec3& from, const Vec3& to) {
        REQUIRE(from == Vec3{0, 7, 0});
        REQUIRE(to.z == 20);
        return blocked;
    };
    CombatBlow fire;
    fire.player = 0;
    fire.damage = 40;
    fire.breath = true;
    fire.origin = Vec3{0, 7, 0};
    LevelOpponents::applyCritterBlow(fire, players, events);
    REQUIRE(hits == 0);
    REQUIRE(players[0].breathGap == 0);
    blocked = false;
    LevelOpponents::applyCritterBlow(fire, players, events);
    REQUIRE(hits == 1);
    REQUIRE(players[0].breathGap == 0.25f);
}

TEST_CASE("the level keeps boss effects on their animated node or full model root",
          "[game][screens][level-opponents][breath][boss-effects][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/DRAGON.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/DRAGON/animations.json");
    s32 kind = 34;
    f32 distance = 25;
    bool rootEffect = false;
    CritterData lichData;
    SECTION("Dragon FIRE rides its animated mouth") {}
    SECTION("Lich attack wind-up rides its elevated root") {
        test::unpackedOrSkip("critter/LICH.json");
        test::unpackedOrSkip("MONSTERS/LICH/animations.json");
        kind = 41;
        distance = 6;
        rootEffect = true;
        REQUIRE(lichData.load(root / "critter/LICH.json"));
    }
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, {}, nullptr, Vec3{0, 0, distance}, 0);
    opponents.open({device, world, weapons, effects, audio, root, 1}, players);
    REQUIRE(opponents.bosses().spawn(kind, Vec3{0}, 0));
    LevelOpponents::Events events;
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    events.blast = [](const Vec3&, f32, f32) {};
    events.settleBlasts = [] {};
    events.legend = [](const LegendEvent&) {};
    events.advanceLegend = [](f32) {};
    events.fallen = [](const Vec3&) {};
    events.spew = [](const CombatSpew&) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    u32 effectId = 0;
    s32 attachedFrames = 0;
    for (s32 tick = 0; tick < 2400 && attachedFrames < 20; ++tick) {
        opponents.update(1, 1.0f / 60, players, {}, events);
        for (usize e = 0; e < effects.count(); ++e) {
            const auto& effect = effects.effect(e);
            const bool rootAttack = effect.name.starts_with("ATK") &&
                                    std::ranges::any_of(lichData.sounds(), [&](const auto& sound) {
                                        return sound.tree == effect.name && (sound.flags & 1U) != 0;
                                    });
            if (rootEffect ? !rootAttack : effect.name != "FIRE") {
                continue;
            }
            effectId = effect.id;
            REQUIRE(effect.attachment.has_value());
            const auto parent = rootEffect ? opponents.bosses().rootTransform()
                                           : opponents.bosses().nodeTransform("NODE#01");
            REQUIRE(parent.has_value());
            CAPTURE(effect.name, effect.position.x, effect.position.y, effect.position.z,
                    (*parent)[3].x, (*parent)[3].y, (*parent)[3].z, effect.scale);
            REQUIRE(effect.transform() == *parent);
            if (!rootEffect) {
                REQUIRE(effect.particles.field().size() == 2);
            }
            ++attachedFrames;
        }
        effects.update(1.0f / 60);
    }
    REQUIRE(attachedFrames == 20);
    REQUIRE(effects.playing(effectId));
    opponents.close();
    REQUIRE_FALSE(effects.playing(effectId));
}
TEST_CASE("the Lich's emergence cue hides the arena mound when he wakes",
          "[game][screens][level-opponents][boss-effects][unpacked]") {
    const auto root = test::unpackedOrSkip("critter/LICH.json").parent_path().parent_path();
    test::unpackedOrSkip("MONSTERS/LICH/animations.json");
    test::unpackedOrSkip("LEVELS/LEVELG5/world.json");
    test::unpackedOrSkip("wdata/TOWN.json");
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G5");
    REQUIRE(level.has_value());
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root, *level));
    const auto& objects = world.layout().objects();
    const auto mound = std::ranges::find(objects, "G5BIGDIRT", &WorldObject::name);
    REQUIRE(mound != objects.end());
    const auto index = static_cast<usize>(std::distance(objects.begin(), mound));
    REQUIRE_FALSE(world.scene().moving(index));
    REQUIRE(world.scene().objectVisible(index));
    const usize triangles = world.collision().triangleCount();
    const Mat4 transform = world.scene().worldTransform(index);
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> players;
    opponents.open({device, world, weapons, effects, audio, root, 1}, {});
    REQUIRE(opponents.bosses().view().kind == 41);
    REQUIRE(opponents.bosses().position() != nullptr);
    players[0].actor.spawn(0, {}, nullptr, *opponents.bosses().position(), 0);
    LevelOpponents::Events events;
    events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    events.blast = [](const Vec3&, f32, f32) {};
    events.settleBlasts = [] {};
    events.legend = [](const LegendEvent&) {};
    events.advanceLegend = [](f32) {};
    events.fallen = [](const Vec3&) {};
    events.spew = [](const CombatSpew&) {};
    events.advanceVictory = [](s32, f32) {};
    events.levels = [] {};
    events.award = [](s32, s32, bool) {};
    opponents.update(2, 1.0f / 30, {}, {}, events);
    REQUIRE(world.scene().objectVisible(index)); // asleep: no callback yet
    opponents.update(2, 1.0f / 30, players, {}, events);
    REQUIRE_FALSE(world.scene().objectVisible(index));
    REQUIRE(world.scene().worldTransform(index) == transform);
    REQUIRE(world.collision().triangleCount() == triangles);
    REQUIRE(world.objectAlpha(index) == 1);
    bool gravel = false;
    for (usize i = 0; i < effects.count(); ++i) {
        gravel |= effects.effect(i).name == "GENFX";
    }
    REQUIRE(gravel);
    REQUIRE_FALSE(world.setObjectVisible("MISSING", false));
    opponents.close();
    effects.clear();
    REQUIRE(world.load(device, root, *level));
    REQUIRE(world.scene().objectVisible(index));
}
} // namespace
