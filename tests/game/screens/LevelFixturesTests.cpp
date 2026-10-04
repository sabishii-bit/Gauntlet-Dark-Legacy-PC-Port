#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeSoundBank.h"
#include "game/enemies/Combatant.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/players/ClassData.h"
#include "game/players/Progression.h"
#include "game/screens/HelpMessages.h"
#include "game/screens/LevelFixtures.h"
#include "game/screens/PlayerHealth.h"
#include "game/world/Chests.h"
#include "game/world/SafeRocks.h"
#include "game/world/Traps.h"
namespace {
using namespace gdl;
using namespace gdl::game;

/** A blast's ring on its first step: a third of its reach, and 1.5 x (1 - 0.33) of its harm. */
constexpr f32 kFirstReach = 0.33f;
constexpr f32 kFirstStep = 1.5f * (1.0f - 0.33f);

struct Fixture {
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelFixtures fixtures;
    std::array<PlayerRuntime, 3> players;
    std::vector<std::string> calls;
    LevelFixtures::Events events{
        .hurt =
            [this](usize i, f32 damage, HurtKind kind, bool directed, const PlayerImpact&) {
                REQUIRE(damage == Catch::Approx(kFirstStep * 5));
                REQUIRE(kind == HurtKind::Blow);
                REQUIRE(directed);
                calls.push_back("player" + std::to_string(i));
            },
        .help =
            [](s32, usize) {
                FAIL("Empty scenery has no help event");
                return false;
            },
        .card = [](s32, std::string_view) { FAIL("Empty scenery has no pickup card"); },
        .opponents =
            [this](const Vec3&, f32 radius, f32 damage, std::vector<s32>&, u32) {
                REQUIRE(radius == Catch::Approx(kFirstReach * 2));
                REQUIRE(damage == Catch::Approx(kFirstStep * 5));
                calls.emplace_back("opponents");
            },
        .releaseEnemy = {},
        .shatterPotion = {}};
    Fixture() {
        fixtures.bind({device, world, weapons, effects, audio, 1});
        players[0].actor.spawn(3, {}, nullptr, Vec3{0}, 0);
        players[1].actor.spawn(1, {}, nullptr, Vec3{100, 0, 100}, 0);
        players[2].actor.spawn(2, {}, nullptr, Vec3{0}, 0);
        players[2].life = PlayerLife::InTower;
    }
};

TEST_CASE("fixture explosions resolve live nearby players before opponents and drain once",
          "[game][screens][level-fixtures]") {
    Fixture f;
    f.fixtures.blast(Vec3{0}, 2, 5, f.players, f.events);
    REQUIRE(f.calls == std::vector<std::string>{"player0", "opponents"});
    f.fixtures.settleBlasts(f.players, f.events);
    REQUIRE(f.calls.size() == 2);
    f.fixtures.clear();
    f.fixtures.blast(Vec3{0}, 2, 5, f.players, f.events);
    REQUIRE(f.calls.size() == 2);
}

TEST_CASE("world explosion fallback keeps its retail radius damage and fire flags",
          "[level-fixtures][world-destruction]") {
    Fixture f;
    usize hits = 0;
    f.events.hurt = [&](usize player, f32 damage, HurtKind kind, bool, const PlayerImpact&) {
        CHECK(player == 0);
        CHECK(damage == Catch::Approx(50 * kFirstStep));
        CHECK(kind == HurtKind::Blow);
        ++hits;
    };
    f.events.opponents = [&](const Vec3&, f32 radius, f32 damage, std::vector<s32>&, u32 flags) {
        CHECK(radius == Catch::Approx(5 * kFirstReach));
        CHECK(damage == Catch::Approx(50 * kFirstStep));
        CHECK(flags == 0x21);
    };
    f.fixtures.worldExplosion({}, f.players, f.events);
    CHECK(hits == 1);
}

TEST_CASE("ice world explosion uses its authored art and poison ring without knockdown",
          "[level-fixtures][world-destruction][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELI1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("ITEMS/LEVELI/ANIM.PS2");
    Fixture f;
    f.fixtures.clear();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("I1");
    REQUIRE(level);
    REQUIRE(f.world.load(f.device, root, *level));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    const Vec3 position{10000, 0, 10000};
    f.players[0].actor.place(position);
    usize hits = 0;
    usize opponentHits = 0;
    f.events.hurt = [&](usize player, f32 damage, HurtKind kind, bool, const PlayerImpact&) {
        CHECK(player == 0);
        CHECK(damage > 0);
        CHECK(kind == HurtKind::Gas);
        ++hits;
    };
    f.events.opponents = [&](const Vec3&, f32 radius, f32, std::vector<s32>& reached, u32 flags) {
        CHECK(flags == 0x800);
        CHECK(radius <= 6);
        if (std::ranges::find(reached, 42) == reached.end()) {
            reached.push_back(42);
            ++opponentHits;
        }
    };
    f.fixtures.worldExplosion(position, f.players, f.events);
    REQUIRE(f.effects.count() == 1);
    CHECK(f.effects.effect(0).name == "WORLD_EXP");
    CHECK(f.effects.effect(0).stretch == Vec3{1});
    f.effects.draw(f.device, Mat4{1}, WorldLighting{});
    REQUIRE_FALSE(f.device.draws.empty());
    REQUIRE_FALSE(f.device.draws.front().vertices.empty());
    const Vec3 firstVertex = f.device.draws.front().vertices.front().position;
    const auto firstDraws = f.device.draws.size();
    f.device.draws.clear();
    f.effects.update(0.3f);
    f.effects.draw(f.device, Mat4{1}, WorldLighting{});
    REQUIRE(f.device.draws.size() >= firstDraws);
    CHECK(glm::distance(f.device.draws.front().vertices.front().position, firstVertex) > 0.1f);
    f.device.draws.clear();
    CHECK(hits == 1);
    f.fixtures.advanceBlasts(0.49f, f.players, f.events);
    CHECK(hits == 1);
    CHECK(opponentHits == 1);
    f.fixtures.advanceBlasts(0.02f, f.players, f.events);
    CHECK(hits == 2);
    CHECK(opponentHits == 2);
    f.fixtures.advanceBlasts(2, f.players, f.events);
    CHECK(hits == 2);
    // Exercise LevelWorld's real authored cart, not just a synthetic animation event.
    constexpr usize kCart = 709;
    REQUIRE(f.world.layout().objects()[kCart].name == "I1MINECART#0");
    for (s32 frame = 0; frame < 184; ++frame) {
        f.world.update(1.0f / 30.0f);
    }
    CHECK_FALSE(f.world.scene().objectVisible(kCart));
    CHECK_FALSE(f.world.collision().solid(static_cast<s32>(kCart)));
    const auto bursts = f.world.takeWorldExplosions();
    REQUIRE(bursts.size() == 1);
    CHECK(bursts[0] == Vec3{f.world.scene().worldTransform(kCart)[3]});
    f.world.update(1.0f / 30.0f);
    CHECK(f.world.scene().objectVisible(kCart));
    CHECK(f.world.collision().solid(static_cast<s32>(kCart)));
    CHECK(f.world.takeWorldExplosions().empty());
}

TEST_CASE("a blast's ring reaches the further out later and for less, each of them once",
          "[game][screens][level-fixtures][blast-ring]") {
    Fixture f;
    f.players[0].actor.place(Vec3{0});
    f.players[1].actor.place(Vec3{6, 0, 0}); // out of the first step's four, in the ring's twelve
    std::vector<std::pair<usize, f32>> hurts;
    std::vector<f32> reaches;
    LevelFixtures::Events events = f.events;
    events.hurt = [&](usize i, f32 damage, HurtKind, bool, const PlayerImpact&) {
        hurts.emplace_back(i, damage);
    };
    events.opponents = [&](const Vec3&, f32 radius, f32, std::vector<s32>&, u32) {
        reaches.push_back(radius);
    };
    f.fixtures.blast(Vec3{0}, 12, 30, f.players, events, 1.0f);
    REQUIRE(hurts.size() == 1);
    CHECK(hurts[0].first == 0);
    CHECK(hurts[0].second == Catch::Approx(kFirstStep * 30));
    for (s32 frame = 0; frame < 40; ++frame) {
        f.fixtures.update(2, 1.0f / 30, f.players, events);
    }
    REQUIRE(hurts.size() == 2); // the second player once, the first never again
    CHECK(hurts[1].first == 1);
    CHECK(hurts[1].second < hurts[0].second);
    CHECK(hurts[1].second > 0.0f);
    // It grows to its whole reach and stops two thirds through its life.
    REQUIRE(reaches.size() > 2);
    CHECK(std::ranges::is_sorted(reaches));
    CHECK(reaches.back() <= 12.0f);
    CHECK(reaches.size() < 25);
}

TEST_CASE("fixture blasts damage pickups within the reduced item radius and emit retail cues",
          "[game][screens][level-fixtures][blast-items][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("WEAPONS/ANIM.PS2");
    Fixture f;
    f.fixtures.clear();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    REQUIRE(f.world.load(f.device, root, *catalog.byName("G1")));
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    const Vec3 origin{10000, 0, 10000};
    const usize first = f.world.placedItems().size();
    REQUIRE(f.world.placeItem(f.device, "APPLE", origin));
    const Mat4 itemTransform = glm::rotate(glm::translate(Mat4{1}, origin), 0.7f, Vec3{0, 1, 0});
    f.world.attachItem(first, itemTransform, false);
    REQUIRE(f.world.placeItem(f.device, "TREAS_GOLD", origin));
    // radius 12 - inset 1.5 + item radius 0.5: 11.1 must remain intact.
    REQUIRE(f.world.placeItem(f.device, "APPLE", origin + Vec3{11.1f, 0, 0}));
    usize helpCount = 0;
    f.events.help = [&](s32 id, usize player) {
        CHECK(id == HelpMessages::kBlastsDestroy);
        CHECK(player == 0);
        ++helpCount;
        return true;
    };
    f.events.opponents = [](const Vec3&, f32, f32, std::vector<s32>&, u32) {};
    const auto party = std::span{f.players}.first(1);
    SECTION("fixture explosion") {
        f.fixtures.blast(origin, 12, 5, party, f.events);
    }
    SECTION("enemy explosion forwards its already expanded step") {
        EnemyMissiles missiles;
        EnemyBlast blast;
        blast.position = origin;
        blast.radius = 12;
        blast.damage = 5;
        blast.flags = 0x421;
        blast.stages = {1};
        missiles.blast(blast);
        missiles.update(1.0f / 30, nullptr, {});
        const auto reaches = missiles.takePickupBlasts();
        REQUIRE(reaches.size() == 1);
        const auto& reached = reaches.front();
        f.fixtures.blastPickups(reached.position, reached.radius, reached.damage, reached.flags,
                                party, f.events);
        CHECK(missiles.takePickupBlasts().empty());
    }
    CHECK(f.world.placedItems().item(first).taken);
    CHECK(f.world.placedItems().item(first + 1).name == "TREAS_JUNK");
    CHECK_FALSE(f.world.placedItems().item(first + 2).taken);
    REQUIRE(f.fixtures.rubble().size() == 1);
    CHECK(f.fixtures.rubble().transform(0) == itemTransform);
    f.device.draws.clear();
    f.fixtures.rubble().draw(f.device, Mat4{1}, f.world.fullLighting());
    REQUIRE_FALSE(f.device.draws.empty());
    const auto slag = f.world.items().models.find(Rubble::kItem);
    REQUIRE(slag.has_value());
    const Mesh& mesh = f.world.items().models.mesh(*slag);
    usize vertices = 0;
    for (const auto& draw : f.device.draws) {
        REQUIRE_FALSE(draw.vertices.empty());
        CHECK(draw.state.depthTest);
        vertices += draw.vertices.size();
        for (const auto& vertex : draw.vertices) {
            CHECK(std::ranges::any_of(mesh.vertices, [&](const MeshVertex& expected) {
                return glm::distance(Vec3{itemTransform * Vec4{expected.position, 1}},
                                     Vec3{vertex.position}) < 0.002f;
            }));
        }
    }
    CHECK(vertices == mesh.triangleCount() * 3);
    REQUIRE(helpCount == 1);
    REQUIRE(f.effects.count() == 4);
    for (usize i = 0; i < f.effects.count(); i += 2) {
        CHECK(f.effects.effect(i).name == "CHESTDEST");
        CHECK(f.effects.effect(i + 1).name == "DESTSMOKE");
    }
    f.effects.update(0.1f);
    f.effects.draw(f.device, Mat4{1}, f.world.fullLighting());
    REQUIRE_FALSE(f.device.draws.empty());
    f.fixtures.settleBlasts(party, f.events);
    CHECK(f.effects.count() == 4);
    CHECK(helpCount == 1);
    f.fixtures.clear();
}

TEST_CASE("Dragon arena vents retain the realm's figures alongside boss-specific items",
          "[game][screens][level-fixtures][boss-stage][assets]") {
    const auto root =
        test::assetOrSkip("ITEMS/LEVELB/ANIM.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("ITEMS/LEVELB6/ANIM.PS2");
    test::assetOrSkip("LEVELS/LEVELB6/WORLDS.PS2");
    test::assetOrSkip("WDATA/MOUNT.WAD");
    Fixture fixture;
    fixture.fixtures.clear();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("B6");
    REQUIRE(level.has_value());
    REQUIRE(fixture.world.load(fixture.device, root, *level));
    REQUIRE(fixture.world.items().trees.find("WIZARD").has_value());
    REQUIRE_FALSE(fixture.world.items().trees.find("FLAMEV").has_value());
    REQUIRE(fixture.world.realmItems().trees.find("FLAMEV").has_value());
    fixture.fixtures.bind(
        {fixture.device, fixture.world, fixture.weapons, fixture.effects, fixture.audio, 1});
    const Traps& traps = fixture.fixtures.traps();
    REQUIRE(traps.size() == 10);
    {
        ItemFigure fire;
        const ItemInstance placement;
        REQUIRE(
            fire.place(fixture.device, fixture.world.realmItems(), "FLAMEV", placement, nullptr));
        fire.play(2, true); // ON: flame particles and the ground crack
        fire.update(1.0f / 30);
        REQUIRE(fire.particles().field().particleCount() > 0);
        fixture.device.draws.clear();
        fire.draw(fixture.device, Mat4{1}, {}, 1, 1, nullptr, TreeModel::Pass::DepthWriting);
        REQUIRE_FALSE(fixture.device.draws.empty());
        for (const auto& draw : fixture.device.draws) {
            CHECK(draw.state.depthWrite);
        }
        fixture.device.draws.clear();
        fire.draw(fixture.device, Mat4{1}, {}, 1, 1, nullptr, TreeModel::Pass::Effects);
        REQUIRE_FALSE(fixture.device.draws.empty());
        for (const auto& draw : fixture.device.draws) {
            CHECK_FALSE(draw.state.depthWrite);
            CHECK(draw.state.depthTest);
            CHECK(draw.state.blend == BlendMode::Additive);
        }
    }
    for (usize i = 0; i < traps.size(); ++i) {
        const ItemFigure& vent = traps.trap(i).figure;
        REQUIRE(vent.hasFigure());
        REQUIRE(vent.sequenceCount() == 4); // OFF, ONA, ON, ONB, not an invented one-tick cycle
        REQUIRE(vent.ticksOf(2) > 1);
        REQUIRE(vent.particles().field().size() > 0);
    }
    fixture.fixtures.clear(); // borrowed figures must go before either archive
    fixture.world.clear();
    REQUIRE_FALSE(fixture.world.realmItems().loaded());
}

TEST_CASE("Courtyard fixtures borrow realm artwork without losing the level archive",
          "[game][screens][level-fixtures][realm-art][assets]") {
    const auto root =
        test::assetOrSkip("ITEMS/LEVELA/ANIM.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("ITEMS/LEVELA1/ANIM.PS2");
    test::assetOrSkip("LEVELS/LEVELA1/WORLDS.PS2");
    Fixture fixture;
    fixture.fixtures.clear();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("A1");
    REQUIRE(level);
    REQUIRE(fixture.world.load(fixture.device, root, *level));
    REQUIRE(fixture.world.items().trees.find("TENTACLE"));
    REQUIRE_FALSE(fixture.world.items().trees.find("CHEST"));
    REQUIRE(fixture.world.realmItems().trees.find("CHEST"));
    fixture.fixtures.bind(
        {fixture.device, fixture.world, fixture.weapons, fixture.effects, fixture.audio, 1});
    const auto& chests = fixture.fixtures.chests();
    const auto& gates = fixture.fixtures.gates();
    const auto& barrels = fixture.fixtures.barrels();
    REQUIRE(chests.size() > 0);
    REQUIRE(gates.size() > 0);
    REQUIRE(barrels.size() > 0);
    for (usize i = 0; i < chests.size(); ++i) {
        CAPTURE(i);
        CHECK(chests.chest(i).figure.hasFigure());
    }
    for (usize i = 0; i < gates.size(); ++i) {
        CAPTURE(i);
        CHECK(gates.gate(i).figure.hasFigure());
    }
    for (usize i = 0; i < barrels.size(); ++i) {
        CAPTURE(i);
        CHECK(barrels.barrel(i).figure.hasFigure());
    }
    fixture.fixtures.clear();
    fixture.world.clear();
}

TEST_CASE("Courtyard tentacles damage a knight at the edge of their sweep only during ON",
          "[level-fixtures][courtyard-tentacles][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELA1/WORLDS.PS2").parent_path().parent_path().parent_path();
    Fixture f;
    f.fixtures.clear();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    REQUIRE(f.world.load(f.device, root, *catalog.byName("A1")));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    f.fixtures.setPlayerCount(1);
    ClassDataSet classes;
    REQUIRE(classes.load(root / "PDATA"));
    const auto* stats = classes.stats(5);
    REQUIRE(stats);
    CharacterSave save;
    save.character = 5;
    save.color = 2;
    save.progress().experience = levelExperience(99);
    save.progress().health = 9999;
    PlayerHealth health;
    PlayerHealth::Events events{};
    events.block = [](f32, f32) {};
    events.sound = [](std::string_view) {};
    events.cry = [](std::string_view) {};
    events.named = [](std::string_view, f32) {};
    usize contacts = 0;
    bool corner = false;
    SECTION("side of the sweep uses the class's full contact width") {}
    SECTION("corner of the sweep expands both box axes") {
        corner = true;
    }
    f.events.hurt = [&](usize player, f32 damage, HurtKind kind, bool directed,
                        const PlayerImpact& impact) {
        ++contacts;
        health.hurt(f.players[player], damage, kind, directed, false,
                    f.world.level()->tuning.damage, events, impact, false, stats);
    };
    f.events.help = [](s32, usize) { return false; };
    for (usize i = 0; i < f.fixtures.traps().size(); ++i) {
        const auto& trap = f.fixtures.traps().trap(i);
        if (trap.subtype != Traps::kBlades || !trap.shown) {
            continue;
        }
        // Native A1's one-player tentacle has a -4 X offset and 4/1 half extents.
        // These points are in its retail contact lane, but outside the former
        // half-width circle test. Resolve their height from the native crossing floor.
        const f32 edge = stats->width * 0.75f;
        const Vec3 local{corner ? -8.0f - edge : -4.0f, 0, -1.0f - edge};
        Vec3 at = Vec3{trap.figure.transform() * Vec4{local, 1}};
        const auto floor = f.world.collision().floorAt(at, 5, 5);
        REQUIRE(floor);
        at.y = floor->y;
        CAPTURE(at.x, at.y, at.z, stats->width);
        f.players[0].actor.spawn(0, save, stats, at, 0);
        REQUIRE_FALSE(trap.box.touchedBy(at, f.players[0].actor.radius(), 0));
        for (s32 frame = 0; frame < 600; ++frame) {
            const usize before = contacts;
            f.fixtures.update(2, 1.0f / 30, std::span{f.players}.first(1), f.events);
            if (contacts > before) {
                CHECK(trap.action == 2);
            }
            if (trap.action == 1 && contacts == 0) {
                CHECK(f.players[0].actor.save().health() == 9999);
            }
        }
        CHECK(f.players[0].actor.save().health() < 9999);
    }
    CHECK(contacts > 0);
}

TEST_CASE("every authored flame trap emits only outside OFF and retains its dying tails",
          "[game][screens][level-fixtures][assets]") {
    const auto root =
        test::assetOrSkip("ITEMS/LEVELB/ANIM.PS2").parent_path().parent_path().parent_path();
    usize checked = 0;
    for (const char realm : std::string_view("BCDGHIJK")) {
        const std::string path = "ITEMS/LEVEL" + std::string(1, realm);
        test::assetOrSkip(path + "/ANIM.PS2");
        ItemArchive archive;
        REQUIRE(archive.load(root / path));
        test::FakeRenderDevice device;
        for (const auto* name : {"FLAMEV", "FLAMEH", "FLAMEV1", "FLAMEH1"}) {
            const auto index = archive.trees.find(name);
            if (!index) {
                continue;
            }
            const auto& tree = archive.trees.tree(*index);
            if (!std::ranges::any_of(tree.nodes,
                                     [](const auto& node) { return node.particle >= 0; })) {
                continue;
            }
            INFO(path << "/" << name);
            ItemFigure figure;
            REQUIRE(figure.place(device, archive, name, {}, nullptr));
            figure.gateParticlesOnSequence(true);
            figure.update(0.1f);
            REQUIRE(figure.particles().field().particleCount() == 0);
            figure.play(2, true);
            figure.update(0.1f);
            REQUIRE(figure.particles().field().particleCount() > 0);
            for (usize i = 0; i < figure.particles().field().size(); ++i) {
                CHECK(figure.particles().field().textureOf(i) != &device.whiteTexture());
            }
            device.draws.clear();
            figure.draw(device, Mat4{1}, {});
            REQUIRE_FALSE(device.draws.empty());
            const usize alive = figure.particles().field().particleCount();
            figure.play(0, true);
            REQUIRE(figure.particles().field().particleCount() == alive);
            for (s32 i = 0; i < 300; ++i) {
                figure.update(1.0f / 30);
            }
            REQUIRE(figure.particles().field().particleCount() == 0);
            figure.play(2, true);
            figure.update(0.1f);
            REQUIRE(figure.particles().field().particleCount() > 0);
            ++checked;
        }
    }
    REQUIRE(checked == 15);
}

TEST_CASE("fixture updates age per-player hazard cooldowns without reordering the party",
          "[game][screens][level-fixtures]") {
    Fixture f;
    f.players[0].hitSoundGap = 3;
    f.players[1].hitSoundGap = 1;
    f.players[0].cloudGap = 0.1f;
    f.fixtures.update(2, 0.2f, f.players, f.events);
    REQUIRE(f.calls.empty());
    REQUIRE(f.players[0].hitSoundGap == 1);
    REQUIRE(f.players[1].hitSoundGap == 0);
    REQUIRE(f.players[0].cloudGap == 0);
    REQUIRE(f.players[0].actor.player() == 3);
    REQUIRE(f.players[1].actor.player() == 1);
}

TEST_CASE("barrel smoke belongs to detonations, not ordinary broken containers",
          "[game][screens][level-fixtures][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("WEAPONS/ANIM.PS2");
    Fixture f;
    f.fixtures.clear();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    REQUIRE(f.world.load(f.device, root, *catalog.byName("G1")));
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    f.fixtures.setPlayerCount(1);
    std::array<bool, 4> checked{};
    for (usize i = 0; i < f.fixtures.barrels().size(); ++i) {
        const auto kind = f.fixtures.barrels().barrel(i).kind;
        const auto index = static_cast<usize>(kind);
        if (checked[index] || !f.fixtures.barrels().standing(i)) {
            continue;
        }
        CAPTURE(kind);
        if (f.fixtures.barrels().barrel(i).health > 1) {
            f.fixtures.strikeBarrel(i, 1, -1, {}, f.events);
            REQUIRE(f.effects.count() == 0);
            REQUIRE(f.fixtures.barrels().standing(i));
        }
        f.fixtures.strikeBarrel(i, 10000, -1, {}, f.events);
        REQUIRE_FALSE(f.fixtures.barrels().standing(i));
        if (kind == BreakableStrike::Kind::Exploding) {
            REQUIRE(f.effects.count() == 2);
            CHECK(f.effects.effect(0).name == "EXPLOSION");
            CHECK(f.effects.effect(1).name == "DESTSMOKE");
        } else if (kind == BreakableStrike::Kind::Poison) {
            REQUIRE(f.effects.count() == 1);
            CHECK(f.effects.effect(0).name == "POISONEXP1");
        } else {
            CHECK(f.effects.count() == 0);
            CHECK(f.fixtures.barrels().barrel(i).state == Breakables::kBreaking);
        }
        checked[index] = true;
        f.effects.clear();
    }
    CHECK(checked == std::array<bool, 4>{true, true, true, true});
    f.fixtures.clear();
}

TEST_CASE("explosions blow chests apart, trapped ones going up in turn, and spent barrels go "
          "leaving their rubble",
          "[game][screens][level-fixtures][rubble][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("WEAPONS/ANIM.PS2");
    Fixture f;
    f.fixtures.clear();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    REQUIRE(f.world.load(f.device, root, *catalog.byName("G1")));
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    f.fixtures.setPlayerCount(1);
    std::vector<s32> helps;
    std::vector<s32> released;
    f.events.help = [&](s32 id, usize) {
        helps.push_back(id);
        return true;
    };
    f.events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    f.events.opponents = [](const Vec3&, f32, f32, std::vector<s32>&, u32) {};
    f.events.releaseEnemy = [&](s32 record, const Vec3&, s32) {
        released.push_back(record);
        return false;
    };
    // A shut chest in an explosion is blown apart, leaving its rubble.
    std::optional<usize> plain;
    for (usize i = 0; i < f.fixtures.chests().size() && !plain; ++i) {
        const auto& chest = f.fixtures.chests().chest(i);
        if (chest.shown && !chest.gone && chest.subtype != Chests::kTrappedChest) {
            plain = i;
        }
    }
    REQUIRE(plain.has_value());
    const auto party = std::span{f.players}.first(1);
    f.players[0].actor.place(Vec3{10000, 0, 10000}); // well away
    const Vec3 at = f.fixtures.chests().chest(*plain).figure.position();
    const usize before = f.fixtures.rubble().size();
    f.fixtures.blast(at, LevelFixtures::kBlastRadius, 30, party, f.events);
    CHECK(f.fixtures.chests().chest(*plain).gone);
    CHECK(f.fixtures.rubble().size() > before);
    CHECK(std::ranges::any_of(released, [](s32 record) { return record >= 0; }));
    // Too weak to break anything apart: under five.
    std::optional<usize> other;
    for (usize i = 0; i < f.fixtures.chests().size() && !other; ++i) {
        const auto& chest = f.fixtures.chests().chest(i);
        if (chest.shown && !chest.gone && chest.subtype != Chests::kTrappedChest &&
            glm::distance(chest.figure.position(), at) > 30.0f) {
            other = i;
        }
    }
    if (other.has_value()) {
        f.fixtures.blast(f.fixtures.chests().chest(*other).figure.position(), 6, 4, party,
                         f.events);
        CHECK_FALSE(f.fixtures.chests().chest(*other).gone);
    }
    // A spent exploding barrel leaves its rubble as it breaks and goes once broken; a player
    // near it is told to shoot such barrels from afar.
    std::optional<usize> red;
    for (usize i = 0; i < f.fixtures.barrels().size() && !red; ++i) {
        if (f.fixtures.barrels().standing(i) &&
            f.fixtures.barrels().barrel(i).kind == BreakableStrike::Kind::Exploding) {
            red = i;
        }
    }
    REQUIRE(red.has_value());
    const Vec3 cask = f.fixtures.barrels().barrel(*red).figure.position();
    f.players[0].actor.place(cask + Vec3{20, 0, 0}); // outside the blast, inside nine? no
    const usize heaps = f.fixtures.rubble().size();
    const usize beforeEffects = f.effects.count();
    f.fixtures.strikeBarrel(*red, 10000, -1, party, f.events);
    // StartExplosion(24): horizontal scale 1.75, raised 2 units (GC 80348168/80348150).
    // Fire and damage share the raised centre; debris smoke stays at the barrel's feet.
    REQUIRE(f.effects.count() == beforeEffects + 2);
    const auto& fireball = f.effects.effect(beforeEffects);
    CHECK(fireball.name == "EXPLOSION");
    CHECK(fireball.position == cask + Vec3(0, 2, 0));
    CHECK(fireball.stretch == Vec3(1.75f, 1, 1.75f));
    CHECK(f.effects.remaining(fireball.id) == Catch::Approx(1));
    CHECK(f.effects.effect(beforeEffects + 1).name == "DESTSMOKE");
    CHECK(f.effects.effect(beforeEffects + 1).position == cask);
    bool raisedDamage = false;
    f.events.opponents = [&](const Vec3& position, f32, f32, std::vector<s32>&, u32) {
        raisedDamage |= position == cask + Vec3(0, 2, 0);
    };
    CHECK(f.fixtures.rubble().size() > heaps);
    CHECK(f.fixtures.barrels().opacityOf(*red) == 1.0f);
    f.players[0].actor.place(cask + Vec3{5, 0, 0});
    helps.clear();
    f32 lowest = 1.0f;
    for (s32 frame = 0; frame < 300 && !f.fixtures.barrels().barrel(*red).gone; ++frame) {
        f.fixtures.update(2, 1.0f / 30, party, f.events);
        lowest = std::min(lowest, f.fixtures.barrels().opacityOf(*red));
    }
    CHECK(f.fixtures.barrels().barrel(*red).gone);
    CHECK(raisedDamage);
    CHECK(lowest < 1.0f); // it faded as it broke
    CHECK(std::ranges::find(helps, HelpMessages::kRedBarrels) != helps.end());
    f.fixtures.clear();
    CHECK(f.fixtures.rubble().size() == 0);
}

TEST_CASE(
    "enemy blast steps destroy nearby chests and detonate barrels without restarting the blast",
    "[level-fixtures][enemy-burst][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    Fixture f;
    f.fixtures.clear();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    REQUIRE(f.world.load(f.device, root, *catalog.byName("G1")));
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    f.fixtures.setPlayerCount(1);
    f.events.help = [](s32, usize) { return true; };
    f.events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    f.events.opponents = [](const Vec3&, f32, f32, std::vector<s32>&, u32) {};
    for (PlayerRuntime& player : f.players) {
        player.actor.place(Vec3{10000});
    }
    EnemyMissiles missiles;
    const EnemyMissiles::SceneryBlast scenery = [&](const PickupBlastReach& step,
                                                    std::vector<s32>& reached) {
        f.fixtures.blastScenery(step.position, step.radius, step.damage, step.flags, f.players,
                                f.events, reached);
    };
    EnemyBlast blast;
    blast.radius = 6;
    blast.damage = 50;
    blast.flags = 0x421;
    blast.stages = {1};
    SECTION("a closed chest is destroyed as the expanding ring reaches it") {
        std::optional<usize> index;
        for (usize i = 0; i < f.fixtures.chests().size() && !index; ++i) {
            const auto& chest = f.fixtures.chests().chest(i);
            if (chest.shown && !chest.gone && chest.subtype != Chests::kTrappedChest) {
                index = i;
            }
        }
        REQUIRE(index);
        const Vec3 at = f.fixtures.chests().chest(*index).figure.position();
        blast.position = at + Vec3{3, 0, 0};
        SECTION("fire destroys the container") {
            missiles.blast(blast);
            for (s32 frame = 0; frame < 61; ++frame) {
                missiles.update(1.0f / 60, nullptr, {}, {}, {}, scenery);
            }
            CHECK(f.fixtures.chests().chest(*index).gone);
            CHECK(f.fixtures.rubble().size() > 0);
        }
        SECTION("gas does not destroy the container") {
            blast.flags = EnemyBlast::kGas;
            missiles.blast(blast);
            for (s32 frame = 0; frame < 61; ++frame) {
                missiles.update(1.0f / 60, nullptr, {}, {}, {}, scenery);
            }
            CHECK_FALSE(f.fixtures.chests().chest(*index).gone);
        }
        SECTION("a chest outside the blast stays intact") {
            blast.position = at + Vec3{20, 0, 0};
            missiles.blast(blast);
            for (s32 frame = 0; frame < 61; ++frame) {
                missiles.update(1.0f / 60, nullptr, {}, {}, {}, scenery);
            }
            CHECK_FALSE(f.fixtures.chests().chest(*index).gone);
        }
    }
    SECTION("a red barrel starts its own explosion once") {
        std::optional<usize> index;
        for (usize i = 0; i < f.fixtures.barrels().size() && !index; ++i) {
            if (f.fixtures.barrels().standing(i) &&
                f.fixtures.barrels().barrel(i).kind == BreakableStrike::Kind::Exploding) {
                index = i;
            }
        }
        REQUIRE(index);
        blast.position = f.fixtures.barrels().barrel(*index).figure.position();
        missiles.blast(blast);
        for (s32 frame = 0; frame < 61; ++frame) {
            missiles.update(1.0f / 60, nullptr, {}, {}, {}, scenery);
        }
        CHECK_FALSE(f.fixtures.barrels().standing(*index));
        usize fireballs = 0;
        for (usize i = 0; i < f.effects.count(); ++i) {
            const auto& effect = f.effects.effect(i);
            if (effect.name == "EXPLOSION" && effect.position == blast.position + Vec3(0, 2, 0)) {
                ++fireballs;
            }
        }
        CHECK(fireballs == 1);
        bool chain = false;
        f.events.opponents = [&](const Vec3& at, f32, f32, std::vector<s32>&, u32) {
            chain |= at == blast.position + Vec3(0, 2, 0);
        };
        f.fixtures.settleBlasts(f.players, f.events);
        CHECK(chain);
    }
    CHECK(missiles.burstCount() == 0);
    f.fixtures.clear();
}

TEST_CASE("a barrel holding Death lets him out when it breaks, instead of a pickup",
          "[game][screens][level-fixtures][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    Fixture f;
    f.fixtures.clear();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    REQUIRE(f.world.load(f.device, root, *catalog.byName("G1")));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    f.fixtures.setPlayerCount(1);
    std::optional<usize> holding;
    for (usize i = 0; i < f.fixtures.barrels().size() && !holding; ++i) {
        if (f.fixtures.barrels().standing(i) &&
            f.fixtures.barrels().barrel(i).kind == BreakableStrike::Kind::Holding) {
            holding = i;
        }
    }
    if (!holding.has_value()) {
        SKIP("G1 has no barrel that holds anything");
    }
    s32 let = -1;
    f.events.help = [](s32, usize) { return true; };
    f.events.releaseEnemy = [&](s32 record, const Vec3&, s32) {
        let = record;
        return true; // as the level's opponents do for Death
    };
    const usize items = f.world.placedItems().size();
    f.fixtures.strikeBarrel(*holding, 10000, -1, {}, f.events);
    CHECK(let >= 0);
    CHECK(f.world.placedItems().size() == items); // nothing dropped in his place
    f.fixtures.clear();
}

TEST_CASE("the swarm's missiles are stopped by rocks, bottles and the triggers that are shot",
          "[game][screens][level-fixtures][enemy-missile-items][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELC3/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("LEVELS/LEVELB6/WORLDS.PS2");
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto count = [](const std::vector<MissileStop>& stops, bool rocks) {
        return std::ranges::count_if(
            stops, [&](const MissileStop& stop) { return (stop.rock >= 0) == rocks; });
    };
    {
        Fixture f;
        f.fixtures.clear();
        REQUIRE(f.world.load(f.device, root, *catalog.byName("C3")));
        f.world.setPlayerCount(4);
        f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
        f.fixtures.setPlayerCount(4);
        const auto& triggers = f.world.triggers();
        const auto shootable =
            std::ranges::count_if(std::views::iota(usize{0}, triggers.size()), [&](usize i) {
                return triggers.trigger(i).enabled && triggers.trigger(i).shootable;
            });
        REQUIRE(shootable > 0);
        const auto potions =
            static_cast<std::ptrdiff_t>(f.world.placedItems().shootablePotions().size());
        const auto stops = f.fixtures.missileStops();
        CHECK(count(stops, false) ==
              static_cast<std::ptrdiff_t>(f.fixtures.obstacles().size()) + shootable + potions);
        CHECK(count(stops, true) == 0);
        for (const MissileStop& stop : stops) {
            CHECK(stop.box.solid);
        }
        f.fixtures.clear();
    }
    Fixture f;
    f.fixtures.clear();
    REQUIRE(f.world.load(f.device, root, *catalog.byName("B6")));
    f.world.setPlayerCount(4);
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    f.fixtures.setPlayerCount(4);
    const SafeRocks& rocks = f.fixtures.safeRocks();
    std::ptrdiff_t standing = 0;
    for (usize i = 0; i < rocks.size(); ++i) {
        standing += rocks.standing(i) ? 1 : 0;
    }
    REQUIRE(standing > 0);
    const auto stops = f.fixtures.missileStops();
    CHECK(count(stops, true) == standing);
    for (const MissileStop& stop : stops) {
        if (stop.rock >= 0) {
            CHECK(stop.rockHealth == rocks.rock(static_cast<usize>(stop.rock)).health);
            CHECK(stop.rockArmor == rocks.rock(static_cast<usize>(stop.rock)).armor);
        }
    }
    f.fixtures.clear();
}

TEST_CASE("the great ones walk through chests and into the barrels they break",
          "[game][screens][level-fixtures][critter-rams][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    Fixture f;
    f.fixtures.clear();
    REQUIRE(f.world.load(f.device, root, *catalog.byName("G1")));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    f.fixtures.setPlayerCount(4);
    const auto items = f.fixtures.critterObstacles();
    CHECK(items.size() == f.fixtures.obstacles().size());
    const Breakables& barrels = f.fixtures.barrels();
    usize breakable = 0;
    usize chests = 0;
    for (const CombatantObstacle& item : items) {
        if (item.kind == CombatantObstacle::Kind::Breakable) {
            REQUIRE(item.id >= 0);
            const Breakables::Barrel& cask = barrels.barrel(static_cast<usize>(item.id));
            CHECK(barrels.standing(static_cast<usize>(item.id)));
            CHECK(item.health == cask.health);
            CHECK(item.explodes == (cask.kind == BreakableStrike::Kind::Exploding));
            ++breakable;
        }
        chests += item.kind == CombatantObstacle::Kind::Chest ? 1 : 0;
    }
    CHECK(breakable == barrels.obstacles().size());
    CHECK(chests == f.fixtures.chests().obstacles().size());
    f.fixtures.clear();
}

TEST_CASE("gas from anywhere spoils the food it reaches and tells the party once",
          "[game][screens][level-fixtures][enemy-gas][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    Fixture f;
    f.fixtures.clear();
    REQUIRE(f.world.load(f.device, root, *catalog.byName("G1")));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    std::vector<s32> helps;
    LevelFixtures::Events events = f.events;
    events.help = [&](s32 id, usize) {
        helps.push_back(id);
        return true;
    };
    const Vec3 spot{200, 0, 200};
    REQUIRE(f.world.placeItem(f.device, "APPLE", spot));
    const usize apple = f.world.placedItems().size() - 1;
    f.fixtures.spoilFood(spot, 3, 2, f.players, events); // two or less spoils nothing
    CHECK(f.world.placedItems().item(apple).name == "APPLE");
    CHECK(helps.empty());
    f.fixtures.spoilFood(spot, 3, 10, f.players, events);
    CHECK(f.world.placedItems().item(apple).name == "GAPPLE");
    CHECK(helps == std::vector<s32>{HelpMessages::kGasSpoils});
    f.fixtures.spoilFood(spot, 3, 10, f.players, events); // spoiled already
    CHECK(helps.size() == 1);
    f.fixtures.clear();
}

TEST_CASE("a tent wall stops the swarm's missiles only while it is raised",
          "[game][screens][level-fixtures][enemy-missile-items][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELD1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    Fixture f;
    f.fixtures.clear();
    REQUIRE(f.world.load(f.device, root, *catalog.byName("D1")));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    f.fixtures.setPlayerCount(4);
    const Traps& traps = f.fixtures.traps();
    std::optional<usize> tent;
    for (usize i = 0; i < traps.size() && !tent; ++i) {
        if (traps.trap(i).shown && traps.trap(i).subtype == Traps::kTentWall) {
            tent = i;
        }
    }
    REQUIRE(tent.has_value());
    const auto raised = [&] {
        const s32 action = traps.trap(*tent).action;
        return action == 1 || action == 2;
    };
    const auto stopping = [&] {
        const Obstacle& box = traps.trap(*tent).box;
        return std::ranges::any_of(f.fixtures.missileStops(), [&](const MissileStop& stop) {
            return stop.box.centre == box.centre && stop.box.halfAcross == box.halfAcross &&
                   stop.box.halfAlong == box.halfAlong;
        });
    };
    bool seenDown = false;
    bool seenUp = false;
    for (s32 frame = 0; frame < 900 && !(seenDown && seenUp); ++frame) {
        f.fixtures.update(2, 1.0f / 30, {}, f.events);
        CHECK(stopping() == raised());
        (raised() ? seenUp : seenDown) = true;
    }
    CHECK(seenUp);
    CHECK(seenDown);
    f.fixtures.clear();
}

TEST_CASE("magic turns Death in a chest into the level's apple and rocks the chest",
          "[game][screens][level-fixtures][death-chest][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    Fixture f;
    f.fixtures.clear();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    REQUIRE(f.world.load(f.device, root, *catalog.byName("G1")));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    f.fixtures.setPlayerCount(4);
    const auto& infos = f.world.layout().itemInfos();
    const Chests& chests = f.fixtures.chests();
    std::optional<usize> death;
    std::optional<usize> plain;
    for (usize i = 0; i < chests.size(); ++i) {
        const s32 inside = chests.chest(i).contents;
        const bool holdsDeath = inside >= 0 && infos[static_cast<usize>(inside)].name == "DEATH";
        (holdsDeath ? death : plain) = i;
    }
    REQUIRE(death.has_value());
    REQUIRE(plain.has_value());
    CHECK_FALSE(f.fixtures.enchantChest(*plain, 10)); // nothing happens to any other chest

    const Mat4 rest = chests.chest(*death).figure.transform();
    REQUIRE(f.fixtures.enchantChest(*death, 10));
    // The first record of that name, a food; three ticks of rocking a point of power.
    CHECK(infos[static_cast<usize>(chests.chest(*death).contents)].name == "APPLE");
    CHECK(chests.chest(*death).contents == 42);
    CHECK(chests.chest(*death).wobble == 30.0f);
    // The subtype lands in the record every chest of that kind shares.
    for (usize i = 0; i < chests.size(); ++i) {
        if (chests.chest(i).info == chests.chest(*death).info) {
            CHECK(chests.chest(i).subtype == Chests::kTransmuted);
        }
    }
    CHECK_FALSE(f.fixtures.enchantChest(*death, 10)); // he is gone
    f.fixtures.update(2, 1.0f / 30, {}, f.events);
    CHECK(chests.chest(*death).figure.transform() != rest);
    for (s32 frame = 0; frame < 20; ++frame) {
        f.fixtures.update(2, 1.0f / 30, {}, f.events);
    }
    CHECK(chests.chest(*death).wobble == 0.0f);
    CHECK(chests.chest(*death).figure.transform() == rest);
    f.fixtures.clear();
}

TEST_CASE("poison barrel cloud remains rendered for the damaging lifetime and disperses",
          "[game][screens][level-fixtures][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("WEAPONS/ANIM.PS2");
    Fixture f;
    f.fixtures.clear();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    REQUIRE(f.world.load(f.device, root, *catalog.byName("G1")));
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    f.fixtures.setPlayerCount(1);
    usize barrel = 0;
    while (barrel < f.fixtures.barrels().size() &&
           f.fixtures.barrels().barrel(barrel).kind != BreakableStrike::Kind::Poison) {
        ++barrel;
    }
    REQUIRE(barrel < f.fixtures.barrels().size());
    f.fixtures.strikeBarrel(barrel, 10000, -1, {}, f.events);
    REQUIRE(f.effects.count() == 1);
    const u32 id = f.effects.effect(0).id;
    for (s32 frame = 0; frame < 119; ++frame) {
        f.effects.update(1.0f / 30);
        f.fixtures.update(2, 1.0f / 30, {}, f.events);
        REQUIRE(f.effects.playing(id));
        if (frame > 30) {
            CHECK(f.effects.effect(0).name == "POISONEXP2");
            CHECK(glm::length(Vec3{f.effects.effect(0).transform()[0]}) == 3.5f);
            f.device.draws.clear();
            f.effects.draw(f.device, Mat4{1}, f.world.fullLighting());
            REQUIRE_FALSE(f.device.draws.empty());
        }
    }
    f.fixtures.update(6, 0.1f, {}, f.events);
    CHECK_FALSE(f.effects.playing(id));
    REQUIRE(f.effects.count() == 1);
    CHECK(f.effects.effect(0).name == "POISONEXP3");
    // The barrel itself is gone once broken, its remains (BARPOI0) left lying.
    CHECK(f.fixtures.barrels().barrel(barrel).gone);
    CHECK(f.fixtures.rubble().size() == 1);
    f.effects.update(2);
    CHECK(f.effects.count() == 0);
    f.fixtures.clear();
}

TEST_CASE("breaking a gas barrel spoils nearby food and announces it only on a change",
          "[game][screens][level-fixtures][poison-food][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("WEAPONS/ANIM.PS2");
    Fixture f;
    f.fixtures.clear();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    REQUIRE(f.world.load(f.device, root, *catalog.byName("G1")));
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    f.fixtures.setPlayerCount(1);
    usize barrel = 0;
    while (barrel < f.fixtures.barrels().size() &&
           (!f.fixtures.barrels().standing(barrel) ||
            f.fixtures.barrels().barrel(barrel).kind != BreakableStrike::Kind::Poison)) {
        ++barrel;
    }
    REQUIRE(barrel < f.fixtures.barrels().size());
    const Vec3 position = f.fixtures.barrels().barrel(barrel).figure.position();
    const auto& items = f.world.placedItems();
    const usize apple = items.size();
    REQUIRE(f.world.placeItem(f.device, "APPLE", position));
    f.players[0].actor.place(position + Vec3{100, 0, 0});
    usize announcements = 0;
    f.events.help = [&](s32 id, usize player) {
        CHECK(id == HelpMessages::kGasSpoils);
        CHECK(player == 0);
        ++announcements;
        return true;
    };
    const auto party = std::span{f.players}.first(1);
    f.fixtures.strikeBarrel(barrel, 10000, -1, {}, f.events);
    CHECK(items.item(apple).name == "APPLE");
    f.fixtures.update(2, 1.0f / 30, party, f.events);
    CHECK(items.item(apple).name == "GAPPLE");
    CHECK(items.item(apple).value == -50);
    REQUIRE(announcements == 1);
    for (s32 frame = 0; frame < 150; ++frame) {
        f.fixtures.update(2, 1.0f / 30, party, f.events);
    }
    CHECK(announcements == 1);
    // The expired cloud must not poison food subsequently dropped in the same spot.
    const usize fresh = items.size();
    REQUIRE(f.world.placeItem(f.device, "CHICKEN", position));
    f.fixtures.update(2, 1.0f / 30, party, f.events);
    CHECK(items.item(fresh).name == "CHICKEN");
    CHECK(items.item(fresh).value == 100);
    f.fixtures.clear();
}

TEST_CASE("Temple platform chest contents can be collected after descending",
          "[level-fixtures][chest-platform][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2").parent_path().parent_path().parent_path();
    Fixture f;
    f.fixtures.clear();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    REQUIRE(f.world.load(f.device, root, *catalog.byName("E1")));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    f.fixtures.setPlayerCount(1);
    f.events.help = [](s32, usize) { return true; };
    f.events.card = [](s32, std::string_view) {};
    f.events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    std::vector<Vec3> initial;
    for (usize i = 0; i < f.fixtures.chests().size(); ++i) {
        initial.push_back(f.fixtures.chests().chest(i).figure.position());
    }
    for (usize i = 0; i < f.world.triggers().size(); ++i) {
        f.world.activateTrigger(f.world.triggers().trigger(i).id, false);
    }
    for (s32 frame = 0; frame < 600; ++frame) {
        f.world.update(1.0f / 60);
        f.world.updateTriggers(1.0f / 60, {});
        f.fixtures.syncFloors();
    }
    usize tested = 0;
    for (usize i = 0; i < initial.size(); ++i) {
        const auto& chest = f.fixtures.chests().chest(i);
        if (!chest.shown || chest.figure.position().y >= initial[i].y - 1) {
            continue;
        }
        CAPTURE(i, chest.instance, chest.figure.position().y, chest.subtype, chest.contents);
        auto& actor = f.players[0].actor;
        actor.place(chest.figure.position() - Vec3{0, ItemFigure::kFloorLift, 0});
        actor.save().progress().inventory.keys = 9;
        const auto party = std::span{f.players}.first(1);
        const s32 gold = actor.save().gold;
        for (s32 frame = 0; frame < 300 && chest.state != Chests::kOpen; ++frame) {
            f.fixtures.update(2, 1.0f / 30, party, f.events);
        }
        REQUIRE(chest.state == Chests::kOpen);
        if (chest.subtype == Chests::kGoldChest) {
            REQUIRE(actor.save().gold == gold);
            REQUIRE_FALSE(chest.gone);
            f.fixtures.update(2, 1.0f / 30, party, f.events);
            const s32 amount =
                f.world.layout().itemInfos()[static_cast<usize>(chest.contents)].value;
            REQUIRE(actor.save().gold == gold + amount);
            REQUIRE_FALSE(chest.gone);
            f.fixtures.update(8, Chests::kCollectedSeconds, party, f.events);
            REQUIRE(chest.gone);
            CHECK(actor.save().gold == gold + amount);
            ++tested;
            continue;
        }
        REQUIRE(chest.held >= 0);
        const auto held = static_cast<usize>(chest.held);
        const auto& item = f.world.placedItems().item(held);
        CAPTURE(item.name, item.position.x, item.position.y, item.position.z, actor.position().x,
                actor.position().y, actor.position().z);
        REQUIRE(item.takeable());
        REQUIRE(f.fixtures.chests().holdingTouchedBy({actor.position(), actor.radius()}) ==
                static_cast<s32>(i));
        const std::array collectors{Collector{item.position, actor.reach(), actor.height() * 0.5f}};
        const auto pickups = f.world.collect(f.device, collectors);
        CHECK(std::ranges::any_of(pickups,
                                  [held](const Pickup& pickup) { return pickup.item == held; }));
        ++tested;
    }
    REQUIRE(tested > 0);
}

TEST_CASE("chest pickups follow NULL1 while opening and cannot be collected early",
          "[game][screens][level-fixtures][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    Fixture f;
    f.fixtures.clear();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    REQUIRE(f.world.load(f.device, root, *catalog.byName("G1")));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    f.fixtures.setPlayerCount(1);
    f.events.help = [](s32, usize) { return true; };
    f.events.card = [](s32, std::string_view) {};
    usize index = 0;
    while (index < f.fixtures.chests().size()) {
        const auto& chest = f.fixtures.chests().chest(index);
        if (chest.shown && chest.subtype == Chests::kChest && chest.contents >= 0 &&
            chest.figure.nodeTransform("NULL1")) {
            break;
        }
        ++index;
    }
    REQUIRE(index < f.fixtures.chests().size());
    const auto& chest = f.fixtures.chests().chest(index);
    f.players[0].actor.place(chest.box.centre);
    f.players[0].actor.save().progress().inventory.keys = 9;
    f.fixtures.update(2, 1.0f / 30, std::span{f.players}.first(1), f.events);
    REQUIRE(chest.state == Chests::kOpening);
    REQUIRE(chest.held >= 0);
    const auto held = static_cast<usize>(chest.held);
    // It remembers who let it out, whose line it is when another takes it (fn_8009F748).
    CHECK(f.world.placedItems().item(held).opener == f.players[0].actor.player());
    const usize count = f.world.placedItems().size();
    REQUIRE_FALSE(f.world.placedItems().item(held).takeable());
    for (s32 frame = 0; frame < 300 && chest.state != Chests::kOpen; ++frame) {
        f.fixtures.update(2, 1.0f / 30, std::span{f.players}.first(1), f.events);
        const auto socket = chest.figure.nodeTransform("NULL1");
        REQUIRE(socket);
        CHECK(glm::distance(f.world.placedItems().item(held).position, Vec3{(*socket)[3]}) <
              0.001f);
    }
    REQUIRE(chest.state == Chests::kOpen);
    CHECK(f.world.placedItems().size() == count);
    CHECK(f.world.placedItems().item(held).takeable());
    CHECK(glm::distance(f.world.placedItems().item(held).position, chest.figure.position()) > 1);
    const Vec3 openedPosition = f.world.placedItems().item(held).position;
    f.fixtures.update(60, 1, std::span{f.players}.first(1), f.events);
    CHECK(f.world.placedItems().item(held).position == openedPosition);
    f.fixtures.clear();
}

TEST_CASE("a trapped chest has a short raised fireball and independently animated debris",
          "[game][level-fixtures][chest-explosion][blast-items][assets]") {
    // StartExplosion(29) starts effect 22 plus a separately oriented effect 29.
    // GC constants at 80348140/803480A0/80348160 are 2.5f/1.0f/3.0 double.
    const auto root =
        test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::assetOrSkip("WEAPONS/ANIM.PS2");
    Fixture f;
    f.fixtures.clear();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    REQUIRE(f.world.load(f.device, root, *catalog.byName("E1")));
    REQUIRE(f.weapons.load(root / "WEAPONS"));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    f.fixtures.setPlayerCount(1);
    f.events.help = [](s32, usize) { return true; };
    f.events.hurt = [](usize, f32, HurtKind, bool, const PlayerImpact&) {};
    f.events.opponents = [](const Vec3&, f32, f32, std::vector<s32>&, u32) {};
    usize index = 0;
    while (index < f.fixtures.chests().size()) {
        const auto& chest = f.fixtures.chests().chest(index);
        if (chest.shown && chest.subtype == Chests::kTrappedChest) {
            break;
        }
        ++index;
    }
    REQUIRE(index < f.fixtures.chests().size());
    const auto& chest = f.fixtures.chests().chest(index);
    const Vec3 origin = chest.figure.position();
    const Mat4 orientation = chest.figure.transform();
    const usize food = f.world.placedItems().size();
    REQUIRE(f.world.placeItem(f.device, "APPLE", origin));
    REQUIRE(f.world.placeItem(f.device, "TREAS_GOLD", origin));
    // Isolate the pickups from the floor query and any nearby authored containers.
    f.world.attachItem(food, glm::translate(Mat4{1}, origin), false);
    f.world.attachItem(food + 1, glm::translate(Mat4{1}, origin), false);
    f.players[0].actor.place(chest.box.centre);
    f.players[0].actor.save().progress().inventory.keys = 1;
    const auto party = std::span{f.players}.first(1);
    for (s32 frame = 0; frame < 150 && !chest.gone; ++frame) {
        f.effects.update(1.0f / 30);
        f.fixtures.update(2, 1.0f / 30, party, f.events);
    }
    REQUIRE(chest.gone);
    CHECK(f.world.placedItems().item(food).taken);
    CHECK(f.world.placedItems().item(food + 1).name == "TREAS_JUNK");
    REQUIRE(f.fixtures.rubble().size() >= 1);
    const auto findEffect = [&](std::string_view name) -> const EffectTrees::Effect* {
        for (usize i = 0; i < f.effects.count(); ++i) {
            if (f.effects.effect(i).name == name) {
                return &f.effects.effect(i);
            }
        }
        return nullptr;
    };
    const auto* fireball = findEffect("EXPLOSION");
    REQUIRE(fireball != nullptr);
    CHECK(fireball->position == origin + Vec3(0, 3, 0));
    CHECK(fireball->stretch == Vec3(2.5f, 1, 2.5f));
    CHECK(f.effects.remaining(fireball->id) == Catch::Approx(1));
    const auto* debris = findEffect("EXPCHEST");
    REQUIRE(debris != nullptr);
    CHECK(debris->transform() == orientation);
    CHECK(f.effects.remaining(debris->id) == Catch::Approx(2.5f));
    f.device.draws.clear();
    fireball->model.draw(f.device, Mat4{1}, fireball->transform(), {}, fireball->pose.matrices());
    REQUIRE_FALSE(f.device.draws.empty());
    CHECK_FALSE(f.device.draws.front().vertices.empty());
    f.effects.update(1.1f);
    CHECK(findEffect("EXPLOSION") == nullptr);
    CHECK(findEffect("EXPCHEST") != nullptr);
    f.fixtures.advanceBlasts(0.75f, party, f.events);
    const usize lateFood = f.world.placedItems().size();
    REQUIRE(f.world.placeItem(f.device, "APPLE", origin));
    f.world.attachItem(lateFood, glm::translate(Mat4{1}, origin), false);
    f.fixtures.advanceBlasts(1.0f / 30, party, f.events);
    CHECK_FALSE(f.world.placedItems().item(lateFood).taken);
    f.device.draws.clear();
    f.fixtures.rubble().draw(f.device, Mat4{1}, f.world.fullLighting());
    CHECK_FALSE(f.device.draws.empty());
    f.fixtures.clear();
}

TEST_CASE("a trapped chest sounds its fuse once on opening, not on a keyless touch",
          "[game][screens][level-fixtures][chest-fuse][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2").parent_path().parent_path().parent_path();
    const auto soundRoot = test::scratchDirectory("chest-fuse-audio");
    const auto bank = soundRoot / "audio/COMMON";
    std::filesystem::create_directories(bank);
    const std::vector<s16> tone(48000, 8192);
    const std::array<test::NativeSoundSample, 1> bankSamples{{{48000, {tone.begin(), tone.end()}}}};
    test::writeNativeSoundBank(
        bank, R"({"sounds":[{"index":0,"name":"S_TICKY","id":0,"sequence":[{"sample":0}]}]})",
        bankSamples);
    AudioMixer mixer(48000);
    SoundPlayer sounds(mixer);
    Fixture f;
    f.fixtures.clear();
    f.audio.open(soundRoot, &sounds, nullptr);
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    REQUIRE(f.world.load(f.device, root, *catalog.byName("E1")));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    f.fixtures.setPlayerCount(1);
    f.events.help = [](s32, usize) { return true; };
    usize index = 0;
    while (index < f.fixtures.chests().size()) {
        const auto& chest = f.fixtures.chests().chest(index);
        if (chest.shown && chest.locked && chest.subtype == Chests::kTrappedChest) {
            break;
        }
        ++index;
    }
    REQUIRE(index < f.fixtures.chests().size());
    const auto& chest = f.fixtures.chests().chest(index);
    f.players[0].actor.place(chest.box.centre);
    const auto party = std::span{f.players}.first(1);
    f.fixtures.update(1, 1.0f / 60, party, f.events);
    CHECK(chest.state == Chests::kShut);
    CHECK(sounds.voiceCount() == 0);
    f.players[0].actor.save().progress().inventory.keys = 1;
    f.fixtures.update(1, 1.0f / 60, party, f.events);
    REQUIRE(chest.state == Chests::kOpening);
    CHECK(sounds.voiceCount() == 1);
    std::array<f32, 1024> output{};
    mixer.mix(output);
    CHECK(output.back() == Catch::Approx(0.25f * std::pow(10.0f, -38.0f / 200.0f)));
    f.fixtures.update(1, 1.0f / 60, party, f.events);
    CHECK(sounds.voiceCount() == 1);
    f.fixtures.clear();
    f.audio.close();
}

TEST_CASE("X-Ray builds visible chest contents without spawning a collectible",
          "[game][level-fixtures][xray][assets]") {
    const auto root = test::assetOrSkip("WDATA/TOWN.WAD").parent_path().parent_path();
    Fixture f;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level);
    f.fixtures.clear();
    REQUIRE(f.world.load(f.device, root, *level));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    f.fixtures.setPlayerCount(1);
    Chests chests;
    REQUIRE(chests.bind(f.device, f.world.layout(), f.world.items(), &f.world.collision()));
    chests.setPlayerCount(1);
    usize index = 0;
    while (index < chests.size() &&
           (!chests.chest(index).shown || chests.chest(index).subtype != Chests::kChest)) {
        ++index;
    }
    REQUIRE(index < chests.size());
    const auto& chest = chests.chest(index);
    std::array party{ChestVisitor{chest.figure.position(), 0.75f, 0, true}};
    const auto pickups = f.world.placedItems().size();
    REQUIRE(chests.updateXray(f.device, f.world.items(), f.world.powerups(), 0, party) == 1);
    CHECK(chest.revealed);
    CHECK(chest.preview.hasFigure());
    CHECK(chest.held == -1);
    CHECK(f.world.placedItems().size() == pickups);
    chests.draw(f.device, Mat4{1}, {});
    bool translucentShell = false;
    for (const auto& draw : f.device.draws) {
        translucentShell |=
            !draw.state.depthWrite && !draw.vertices.empty() && draw.vertices.front().color.a == 63;
    }
    CHECK(translucentShell);
    party[0].xray = false;
    chests.updateXray(f.device, f.world.items(), f.world.powerups(), 0, party);
    CHECK_FALSE(chest.revealed);
}
TEST_CASE("Temple entrance preserves keys and X-Ray reveals its actual container figures",
          "[game][level-fixtures][temple-inventory][xray][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELE1/WORLDS.PS2").parent_path().parent_path().parent_path();
    Fixture f;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("E1");
    REQUIRE(level);
    f.fixtures.clear();
    REQUIRE(f.world.load(f.device, root, *level));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    f.fixtures.setPlayerCount(1);
    f.events.help = [](s32, usize) { return true; };
    auto& inventory = f.players[0].actor.save().progress().inventory;
    inventory.keys = 1;
    inventory.addPowerup(9, 2, 0, 60);
    const auto party = std::span{f.players}.first(1);
    SECTION("floor-only carpet is not a key-operated gate") {
        const auto& instances = f.world.layout().itemInstances();
        const auto gate = std::ranges::find_if(
            instances, [](const auto& instance) { return instance.name == "E1DOORCARPET23"; });
        REQUIRE(gate != instances.end());
        REQUIRE(gate->collision.size() == 6);
        for (const auto& triangle : gate->collision) {
            CHECK(triangle.normal == Vec3{0, 1, 0});
        }
        f.players[0].actor.place(gate->position);
        f.fixtures.update(1, 1.0f / 60, party, f.events);
        CHECK(inventory.keys == 1);
    }
    SECTION("food, treasure and trapped chest previews actually draw") {
        std::array<bool, 3> checked{};
        for (usize i = 0; i < f.fixtures.chests().size(); ++i) {
            const auto& chest = f.fixtures.chests().chest(i);
            if (!chest.shown || chest.contents < 0) {
                continue;
            }
            const auto& record = f.world.layout().itemInfos()[static_cast<usize>(chest.contents)];
            usize kind = 0;
            if (chest.subtype == Chests::kTrappedChest) {
                kind = 2;
            } else if (chest.subtype == Chests::kGoldChest) {
                kind = 1;
            }
            if (checked[kind] || record.type != ItemInfo::kPowerup ||
                (kind == 0 && record.subtype != 3)) {
                continue;
            }
            CAPTURE(chest.instance, record.name);
            // Inside the ten-unit reveal radius, outside the chest's touch box.
            f.players[0].actor.place(chest.figure.position() + Vec3{0, 0, 4});
            f.fixtures.update(1, 1.0f / 60, party, f.events);
            REQUIRE(chest.state == Chests::kShut);
            CHECK(chest.revealed);
            CHECK(chest.preview.hasFigure());
            f.device.draws.clear();
            chest.preview.draw(f.device, Mat4{1}, {});
            CHECK_FALSE(f.device.draws.empty());
            CHECK(inventory.keys == 1);
            // Toggling off restores the container instead of leaving a stale reveal.
            inventory.powerups[0].on = false;
            f.fixtures.update(1, 1.0f / 60, party, f.events);
            CHECK_FALSE(chest.revealed);
            inventory.powerups[0].on = true;
            checked[kind] = true;
        }
        CHECK(checked == std::array<bool, 3>{true, true, true});
    }
    f.fixtures.clear();
}

TEST_CASE("a keyless touch of a silver chest falls back to its own hint",
          "[game][items][level-fixtures][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELA6/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("A6");
    REQUIRE(level);
    for (const bool lessonShown : {true, false}) {
        CAPTURE(lessonShown);
        Fixture f;
        f.fixtures.clear();
        REQUIRE(f.world.load(f.device, root, *level));
        f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
        const Chests& chests = f.fixtures.chests();
        usize silver = chests.size();
        for (usize i = 0; i < chests.size(); ++i) {
            if (chests.chest(i).subtype == Chests::kRandomChest && chests.chest(i).locked) {
                silver = i;
            }
        }
        REQUIRE(silver < chests.size());
        f.players[0].actor.spawn(0, {}, nullptr, chests.chest(silver).figure.position(), 0);
        f.players[1].life = PlayerLife::InTower;
        std::vector<s32> posted;
        LevelFixtures::Events events = f.events;
        events.help = [&](s32 id, usize) {
            posted.push_back(id);
            return id != HelpMessages::kChestNeedsKey || lessonShown;
        };
        for (s32 i = 0; i < 5 && posted.empty(); ++i) {
            f.fixtures.update(2, 1.0f / 30, f.players, events);
        }
        if (lessonShown) {
            CHECK(posted == std::vector<s32>{HelpMessages::kChestNeedsKey});
        } else {
            CHECK(posted ==
                  std::vector<s32>{HelpMessages::kChestNeedsKey, HelpMessages::kRandomChest});
        }
        f.fixtures.clear();
    }
}

TEST_CASE("traps pass under a levitating character", "[game][items][level-fixtures][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level);
    for (const bool levitating : {false, true}) {
        CAPTURE(levitating);
        Fixture f;
        f.fixtures.clear();
        REQUIRE(f.world.load(f.device, root, *level));
        f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
        REQUIRE(f.fixtures.traps().size() > 0);
        const Vec3 spot = f.fixtures.traps().trap(0).figure.position();
        f.players[0].actor.spawn(0, {}, nullptr, spot, 0);
        f.players[1].life = PlayerLife::InTower;
        if (levitating) {
            f.players[0].actor.save().progress().inventory.addPowerup(powerup::kSpecial,
                                                                      powerup::kLevitation, 0, 600);
        }
        s32 hurts = 0;
        LevelFixtures::Events events = f.events;
        events.hurt = [&hurts](usize, f32, HurtKind, bool, const PlayerImpact&) { ++hurts; };
        events.help = [](s32, usize) { return true; };
        for (s32 i = 0; i < 600; ++i) {
            f.fixtures.update(2, 1.0f / 30, f.players, events);
        }
        CHECK((hurts > 0) != levitating);
        f.fixtures.clear();
    }
}

TEST_CASE("Castle light traps zap on contact, not activation or cooldown frames",
          "[game][level-fixtures][trap-hit-audio][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELA1/WORLDS.PS2").parent_path().parent_path().parent_path();
    const auto soundRoot = test::scratchDirectory("castle-light-trap-audio");
    const auto bank = soundRoot / "audio/COMMON";
    std::filesystem::create_directories(bank);
    const std::array<test::NativeSoundSample, 1> samples{{{48000, std::vector<s16>(48000, 8192)}}};
    test::writeNativeSoundBank(
        bank, R"({"sounds":[{"index":0,"name":"S_FFIELDZAPA","id":0,"sequence":[{"sample":0}]}]})",
        samples);
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("A1");
    REQUIRE(level);
    for (const bool protectedBody : {false, true}) {
        CAPTURE(protectedBody);
        AudioMixer mixer(48000);
        SoundPlayer sounds(mixer);
        Fixture f;
        f.fixtures.clear();
        f.audio.open(soundRoot, &sounds, nullptr);
        REQUIRE(f.world.load(f.device, root, *level));
        f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
        f.fixtures.setPlayerCount(1);
        const Traps& traps = f.fixtures.traps();
        usize index = 0;
        while (index < traps.size() &&
               (!traps.trap(index).shown || traps.trap(index).subtype != 2)) {
            ++index;
        }
        REQUIRE(index < traps.size());
        const auto& trap = traps.trap(index);
        auto& player = f.players[0];
        player.actor.spawn(0, {}, nullptr, Vec3{0, -1.0e6f, 0}, 0);
        player.actor.save().progress().health = 1000;
        if (protectedBody) {
            player.actor.save().progress().inventory.addPowerup(6, 0x200, 0, 600);
        }
        const auto party = std::span{f.players}.first(1);
        PlayerHealth health;
        const PlayerHealth::Events healthEvents{.block = [](f32, f32) {},
                                                .sound = [](std::string_view) {},
                                                .cry = [](std::string_view) {},
                                                .named = [](std::string_view, f32) {},
                                                .learnBlock = [] {}};
        usize hits = 0;
        f.events.hurt = [&](usize victim, f32 damage, HurtKind kind, bool directed,
                            const PlayerImpact& impact) {
            CHECK(victim == 0);
            health.hurt(player, damage, kind, directed, false, 1, healthEvents, impact);
            ++hits;
        };
        f.events.help = [](s32, usize) { return true; };
        for (s32 frame = 0; frame < 1200 && trap.action != 2; ++frame) {
            f.fixtures.update(1, 1.0f / 60, party, f.events);
        }
        REQUIRE(trap.action == 2);
        CHECK(hits == 0);
        CHECK(sounds.voiceCount() == 0);
        player.actor.place(trap.box.centre);
        f.fixtures.update(0, 0, party, f.events);
        REQUIRE(hits == 1);
        CHECK((player.actor.save().health() < 1000) == !protectedBody);
        REQUIRE(sounds.voiceCount() == 1);
        std::array<f32, 1024> output{};
        mixer.mix(output);
        CHECK(std::ranges::any_of(output, [](f32 sample) { return sample != 0; }));
        sounds.stopAll();
        mixer.mix(output);
        sounds.update();
        f.fixtures.update(0, 0, party, f.events);
        CHECK(hits == 1);
        CHECK(sounds.voiceCount() == 0);
        for (s32 frame = 0; frame < 1200 && hits == 1; ++frame) {
            f.fixtures.update(1, 1.0f / 60, party, f.events);
        }
        CHECK(hits == 2);
        CHECK(sounds.voiceCount() == 1);
        f.fixtures.clear();
        f.audio.close();
    }
}

TEST_CASE("native trap properties reach player damage and reactions",
          "[game][items][level-fixtures][trap-impact][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELA1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    struct Case {
        std::string_view level;
        s32 subtype;
        u32 properties;
        u32 protection;
    };
    for (const auto& trial :
         {Case{"A1", 4, 0x20, 0x40000}, Case{"A6", 5, 0x20, 0x40000}, Case{"B1", 1, 1, 0x100},
          Case{"A1", 2, 2, 0x200}, Case{"A1", 0, 0x2000, 0x10000}}) {
        for (const bool protectedBody : {false, true}) {
            CAPTURE(trial.level, trial.subtype, protectedBody);
            Fixture f;
            f.fixtures.clear();
            const auto level = catalog.byName(trial.level);
            REQUIRE(level);
            REQUIRE(f.world.load(f.device, root, *level));
            f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
            f.fixtures.setPlayerCount(1);
            const Traps& traps = f.fixtures.traps();
            usize chosen = traps.size();
            for (usize i = 0; i < traps.size(); ++i) {
                if (traps.trap(i).shown && traps.trap(i).subtype == trial.subtype) {
                    chosen = i;
                    break;
                }
            }
            REQUIRE(chosen < traps.size());
            const auto& trap = traps.trap(chosen);
            REQUIRE(trap.properties == trial.properties);
            auto& player = f.players[0];
            player.actor.spawn(0, {}, nullptr, trap.box.centre, 0);
            player.actor.save().progress().health = 1000;
            f.players[1].life = PlayerLife::InTower;
            if (protectedBody) {
                player.actor.save().progress().inventory.addPowerup(6, trial.protection, 0, 600);
            }
            PlayerHealth health;
            const PlayerHealth::Events healthEvents{.block = [](f32, f32) {},
                                                    .sound = [](std::string_view) {},
                                                    .cry = [](std::string_view) {},
                                                    .named = [](std::string_view, f32) {},
                                                    .learnBlock = [] {}};
            usize hits = 0;
            f.events.hurt = [&](usize victim, f32 damage, HurtKind kind, bool directed,
                                const PlayerImpact& impact) {
                CHECK(victim == 0);
                CHECK(impact.flags == (trial.properties | PlayerImpact::kStun));
                CHECK(directed == (trial.properties == PlayerImpact::kKnockDown));
                if (directed) {
                    CHECK(glm::distance(impact.direction, -Vec3{trap.figure.transform()[2]}) <
                          0.0001f);
                }
                health.hurt(player, damage, kind, directed, false, 1, healthEvents, impact);
                ++hits;
            };
            f.events.help = [](s32, usize) { return true; };
            for (s32 frame = 0; frame < 1200 && hits == 0; ++frame) {
                f.fixtures.update(2, 1.0f / 30, f.players, f.events);
            }
            REQUIRE(hits == 1);
            const bool heavy = trial.properties == PlayerImpact::kKnockDown;
            CHECK((player.actor.save().health() < 1000) == (!protectedBody || heavy));
            if (heavy && !protectedBody) {
                CHECK((player.reaction == PlayerDeed::FallForward ||
                       player.reaction == PlayerDeed::FallBack));
                CHECK(player.knockback.pending());
                player.knockback.kick(0, false);
                CHECK(glm::length(player.knockback.velocity()) > 0);
            } else {
                // Stun/spike flags can remain queued without imparting any velocity.
                player.knockback.kick(0, false);
                CHECK(player.knockback.velocity() == Vec3{0});
                CHECK(player.reaction != PlayerDeed::FallForward);
                CHECK(player.reaction != PlayerDeed::FallBack);
                if (!protectedBody && trial.properties == PlayerImpact::kSpike) {
                    CHECK(player.reaction == PlayerDeed::Spike);
                }
            }
        }
    }
}

TEST_CASE("armor items prevent fixture knockdown before the health callback",
          "[game][items][level-fixtures][assets]") {
    Fixture f;
    const auto root =
        test::assetOrSkip("LEVELS/LEVELG1/WORLDS.PS2").parent_path().parent_path().parent_path();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("G1");
    REQUIRE(level);
    REQUIRE(f.world.load(f.device, root, *level));
    REQUIRE_FALSE(f.world.isTower());
    f.fixtures.blast(Vec3{0}, 2, 5, f.players, f.events);
    REQUIRE(f.players[0].reaction != PlayerDeed::None);
    for (const u32 flags : {0x10000U, 0x110000U, 0x40000U}) {
        f.players[0].actor.save().progress().inventory = {};
        f.players[0].actor.save().progress().inventory.addPowerup(6, flags, 0, 20);
        f.players[0].reaction = PlayerDeed::None;
        f.fixtures.blast(Vec3{0}, 2, 5, f.players, f.events);
        CHECK(f.players[0].reaction == PlayerDeed::None);
    }
}
} // namespace
