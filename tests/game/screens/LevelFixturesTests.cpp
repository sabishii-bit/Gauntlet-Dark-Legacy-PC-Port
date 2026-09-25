#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/LevelFixtures.h"
namespace {
using namespace gdl;
using namespace gdl::game;

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
            [this](usize i, f32 damage, HurtKind kind, bool directed) {
                REQUIRE(damage == 5);
                REQUIRE(kind == HurtKind::Blow);
                REQUIRE(directed);
                calls.push_back("player" + std::to_string(i));
            },
        .help = [](s32, usize) { FAIL("Empty scenery has no help event"); },
        .card = [](s32, std::string_view) { FAIL("Empty scenery has no pickup card"); },
        .opponents =
            [this](const Vec3&, f32 radius, f32 damage) {
                REQUIRE(radius == 2);
                REQUIRE(damage == 5);
                calls.emplace_back("opponents");
            }};
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

TEST_CASE("Dragon arena vents retain the realm's figures alongside boss-specific items",
          "[game][screens][level-fixtures][boss-stage][unpacked]") {
    const auto root = test::unpackedOrSkip("ITEMS/LEVELB/animations.json")
                          .parent_path()
                          .parent_path()
                          .parent_path();
    test::unpackedOrSkip("ITEMS/LEVELB6/animations.json");
    test::unpackedOrSkip("LEVELS/LEVELB6/world.json");
    test::unpackedOrSkip("wdata/MOUNT.json");
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

TEST_CASE("every authored flame trap emits only outside OFF and retains its dying tails",
          "[game][screens][level-fixtures][unpacked]") {
    const auto root = test::unpackedOrSkip("ITEMS/LEVELB/animations.json")
                          .parent_path()
                          .parent_path()
                          .parent_path();
    usize checked = 0;
    for (const char realm : std::string_view("BCDGHIJK")) {
        const std::string path = "ITEMS/LEVEL" + std::string(1, realm);
        test::unpackedOrSkip(path + "/animations.json");
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
          "[game][screens][level-fixtures][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELG1/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("WEAPONS/animations.json");
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

TEST_CASE("poison barrel cloud remains rendered for the damaging lifetime and disperses",
          "[game][screens][level-fixtures][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELG1/world.json").parent_path().parent_path().parent_path();
    test::unpackedOrSkip("WEAPONS/animations.json");
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
    CHECK_FALSE(f.fixtures.barrels().barrel(barrel).gone);
    CHECK(f.fixtures.barrels().barrel(barrel).state == Breakables::kBroken);
    f.effects.update(2);
    CHECK(f.effects.count() == 0);
    f.fixtures.clear();
}

TEST_CASE("chest pickups follow NULL1 while opening and cannot be collected early",
          "[game][screens][level-fixtures][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELG1/world.json").parent_path().parent_path().parent_path();
    Fixture f;
    f.fixtures.clear();
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    REQUIRE(f.world.load(f.device, root, *catalog.byName("G1")));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    f.fixtures.setPlayerCount(1);
    f.events.help = [](s32, usize) {};
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

TEST_CASE("X-Ray builds visible chest contents without spawning a collectible",
          "[game][level-fixtures][xray][unpacked]") {
    const auto root = test::unpackedOrSkip("wdata/TOWN.json").parent_path().parent_path();
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
          "[game][level-fixtures][temple-inventory][xray][unpacked]") {
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELE1/world.json").parent_path().parent_path().parent_path();
    Fixture f;
    LevelCatalog catalog;
    REQUIRE(catalog.load(root));
    const auto level = catalog.byName("E1");
    REQUIRE(level);
    f.fixtures.clear();
    REQUIRE(f.world.load(f.device, root, *level));
    f.fixtures.bind({f.device, f.world, f.weapons, f.effects, f.audio, 1});
    f.fixtures.setPlayerCount(1);
    f.events.help = [](s32, usize) {};
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

TEST_CASE("armor items prevent fixture knockdown before the health callback",
          "[game][items][level-fixtures][unpacked]") {
    Fixture f;
    const auto root =
        test::unpackedOrSkip("LEVELS/LEVELG1/world.json").parent_path().parent_path().parent_path();
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
