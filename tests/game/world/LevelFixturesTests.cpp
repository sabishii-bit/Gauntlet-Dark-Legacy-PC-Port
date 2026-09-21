#include <array>
#include <filesystem>
#include <numbers>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/world/Breakables.h"
#include "game/world/Chests.h"
#include "game/world/ItemFigure.h"
#include "game/world/LockedGates.h"
#include "game/world/Traps.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

constexpr f32 kPi = std::numbers::pi_v<f32>;

/** A level with a locked chest of potions-or-keys at the origin, a chest of gold at x 20, a
 * trapped one at x 40, a barrel at x 60, a gate across x 80 and spikes at x 100. */
std::filesystem::path sampleLevel(std::string_view name) {
    const auto dir = test::scratchDirectory(name);
    writeTextFile(dir / "world.json", R"({
  "objects": [{"name": "GROUND", "position": [0, 0, 0], "next": -1, "child": -1}],
  "animations": [], "particles": [], "locators": [],
  "itemInfos": [
    {"type": 2, "subtype": 46, "name": "CHEST", "radius": 3.9, "height": 2, "xSize": 1.2,
     "zSize": 1, "activeType": 22},
    {"type": 1, "subtype": 2, "name": "KEY", "radius": 0.5, "height": 2, "value": 1},
    {"type": 1, "subtype": 4, "name": "POT_RED", "radius": 0.5, "height": 2, "value": 1,
     "properties": 1},
    {"type": -1, "subtype": 2, "name": "", "choices": [1, 2]},
    {"type": 2, "subtype": 47, "name": "CHESTG0", "radius": 3.9, "height": 2, "xSize": 1.2,
     "zSize": 1, "activeType": 22},
    {"type": 1, "subtype": 1, "name": "TREAS_GOLD", "radius": 1.25, "height": 2, "value": 200},
    {"type": 2, "subtype": 44, "name": "CHESTEXP", "radius": 3.9, "height": 2, "xSize": 1.2,
     "zSize": 1, "activeType": 22},
    {"type": 2, "subtype": 43, "name": "BAROBJ", "radius": 1, "height": 3, "armor": 1,
     "hitPoints": 5, "activeType": 518},
    {"type": 7, "subtype": 3, "name": "GATED", "radius": 3.7, "height": 5, "xSize": 3.5,
     "zSize": 1, "activeType": 70},
    {"type": 8, "subtype": 0, "name": "SPIKES", "radius": 3.5, "height": 5, "xSize": 2,
     "zSize": 0.4, "value": 20, "activeType": 5, "activeOff": -40},
    {"type": 10, "subtype": 44, "name": "BAREXP", "radius": 1, "height": 3, "armor": 1,
     "hitPoints": 5, "activeType": 6},
    {"type": 10, "subtype": 45, "name": "BARPOI", "radius": 1, "height": 3, "armor": 1,
     "hitPoints": 5, "activeType": 6},
    {"type": 10, "subtype": 43, "name": "BAROBJ", "radius": 1, "height": 3, "armor": 1,
     "hitPoints": 5, "activeType": 6}],
  "itemInstances": [
    {"info": 0, "minPlayers": 1, "position": [0, 0, 0], "rotation": [0, 0, 0],
     "params": [3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]},
    {"info": 4, "minPlayers": 1, "position": [20, 0, 0], "rotation": [0, 0, 0],
     "params": [5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]},
    {"info": 6, "minPlayers": 2, "position": [40, 0, 0], "rotation": [0, 0, 0],
     "params": [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]},
    {"info": 7, "minPlayers": 1, "position": [60, 0, 0], "rotation": [0, 0, 0],
     "params": [1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]},
    {"info": 8, "minPlayers": 1, "position": [80, 0, 0], "rotation": [0, 1.5707964, 0],
     "params": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]},
    {"info": 9, "minPlayers": 1, "position": [100, 0, 0], "rotation": [0, 0, 0],
     "params": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]},
    {"info": 10, "minPlayers": 1, "position": [200, 0, 0], "rotation": [0, 0, 0],
     "params": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]},
    {"info": 11, "minPlayers": 1, "position": [203, 0, 0], "rotation": [0, 0, 0],
     "params": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]},
    {"info": 12, "minPlayers": 3, "position": [220, 0, 0], "rotation": [0, 0, 0],
     "params": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]}]
})");
    return dir;
}

struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ItemArchive items; ///< left unloaded: everything works unseen

    explicit Fixture(std::string_view name) { REQUIRE(layout.load(sampleLevel(name))); }
};

TEST_CASE("a box pushes a body out by its nearest side and knows what is against it",
          "[game][world][fixtures]") {
    Obstacle box;
    box.centre = Vec3{10.0f, 0.0f, 5.0f};
    box.halfAcross = 2.0f;
    box.halfAlong = 1.0f;
    box.height = 2.0f;
    // Clear of it, a body stays; against it, it is put a radius off the side.
    REQUIRE(box.pushOut(Vec3{20.0f, 0.0f, 5.0f}, 0.75f) == Vec3{20.0f, 0.0f, 5.0f});
    const Vec3 side = box.pushOut(Vec3{12.2f, 0.0f, 5.0f}, 0.75f);
    REQUIRE(side.x == Approx(12.75f));
    REQUIRE(side.z == Approx(5.0f));
    const Vec3 inside = box.pushOut(Vec3{10.0f, 0.0f, 5.6f}, 0.75f);
    REQUIRE(inside.z == Approx(6.75f)); // out by the nearer, long side
    REQUIRE(inside.x == Approx(10.0f));
    // Over it, it is not in the way.
    REQUIRE(box.pushOut(Vec3{10.0f, 9.0f, 5.0f}, 0.75f).y == 9.0f);
    REQUIRE(box.touchedBy(Vec3{12.9f, 0.0f, 5.0f}, 0.75f));
    REQUIRE_FALSE(box.touchedBy(Vec3{13.2f, 0.0f, 5.0f}, 0.75f));
    REQUIRE_FALSE(box.touchedBy(Vec3{12.9f, 9.0f, 5.0f}, 0.75f));
    // Turned a quarter, its long way lies along z.
    box.yaw = 1.5707964f;
    REQUIRE(box.touchedBy(Vec3{10.0f, 0.0f, 7.5f}, 0.75f));
    REQUIRE_FALSE(box.touchedBy(Vec3{12.5f, 0.0f, 5.0f}, 0.75f));
    box.solid = false;
    REQUIRE(box.pushOut(Vec3{10.0f, 0.0f, 5.0f}, 0.75f) == Vec3{10.0f, 0.0f, 5.0f});
    // A half turn of pitch and of roll together is a half turn of yaw.
    const Mat4 flipped = itemPlacement(Vec3{1.0f, 2.0f, 3.0f}, Vec3{kPi, 0.5f, -kPi});
    const Mat4 turned = itemPlacement(Vec3{1.0f, 2.0f, 3.0f}, Vec3{0.0f, 0.5f + kPi, 0.0f});
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            REQUIRE(flipped[column][row] == Approx(turned[column][row]).margin(1e-5));
        }
    }
    REQUIRE(shownToParty(1, 1));
    REQUIRE_FALSE(shownToParty(3, 2));
    REQUIRE(shownToParty(12, 2)); // exactly two
    REQUIRE_FALSE(shownToParty(12, 3));
}

TEST_CASE("a chest's contents are its record, or the pick from a list by the item's place",
          "[game][world][fixtures]") {
    const Fixture f("fixtures-contents");
    u32 seed = 0;
    REQUIRE(Chests::resolveContents(f.layout.itemInfos(), 5, 0, seed) == 5);
    REQUIRE(seed == 0U);
    REQUIRE(Chests::resolveContents(f.layout.itemInfos(), 3, 0, seed) == 1); // (0 + 0) % 2
    REQUIRE(seed == static_cast<u32>(Chests::kSeedStep));
    REQUIRE(Chests::resolveContents(f.layout.itemInfos(), 3, 1, seed) == 1); // (13 + 1) % 2
    REQUIRE(Chests::resolveContents(f.layout.itemInfos(), 3, 0, seed) == 2); // (27 + 0) % 2
    REQUIRE(Chests::resolveContents(f.layout.itemInfos(), -1, 0, seed) == -1);
    REQUIRE(Chests::resolveContents(f.layout.itemInfos(), 99, 0, seed) == -1);
}

TEST_CASE("a locked chest wants a key, opens, and gives up what it held",
          "[game][world][fixtures]") {
    Fixture f("fixtures-chests");
    Chests chests;
    REQUIRE(chests.bind(f.device, f.layout, f.items, nullptr));
    REQUIRE(chests.size() == 3); // the barrel is not a chest
    chests.setPlayerCount(1);
    REQUIRE(chests.chest(0).locked);
    REQUIRE(chests.chest(0).box.halfAcross == 1.2f);
    REQUIRE_FALSE(chests.chest(2).shown); // the trapped one is for two players
    REQUIRE(chests.obstacles().size() == 2);

    std::array<ChestVisitor, 1> party{ChestVisitor{Vec3{10.0f, 0.0f, 0.0f}, 0.75f, 0}};
    REQUIRE(chests.update(1.0f / 30.0f, party).empty()); // nobody near
    party[0].position = Vec3{1.9f, 0.0f, 0.0f};
    std::vector<ChestEvent> events = chests.update(1.0f / 30.0f, party);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].kind == ChestEvent::Kind::Refused);
    REQUIRE(chests.update(1.0f / 30.0f, party).empty()); // not told again at once
    party[0].keys = 1;
    events = chests.update(1.0f / 30.0f, party);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].kind == ChestEvent::Kind::Unlocked);
    REQUIRE(events[0].chest == 0);
    REQUIRE(chests.chest(0).state == Chests::kOpening);
    events = chests.update(1.0f / 30.0f, party);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].kind == ChestEvent::Kind::Opened);
    REQUIRE(events[0].contents == 1); // the list's first pick: a key
    REQUIRE(events[0].gold == 0);
    REQUIRE(chests.chest(0).state == Chests::kOpen);
    REQUIRE(chests.update(1.0f / 30.0f, party).empty()); // open, it has no more to give
    // What came out lies in it, reached by touching it; emptied, the chest goes.
    REQUIRE(chests.holdingTouchedBy(party[0]) == -1);
    chests.hold(0, 7);
    REQUIRE(chests.holdingTouchedBy(party[0]) == 0);
    REQUIRE(chests.holdingTouchedBy(ChestVisitor{Vec3{10.0f, 0.0f, 0.0f}, 0.75f, 0}) == -1);
    chests.remove(0);
    REQUIRE(chests.chest(0).gone);
    REQUIRE(chests.holdingTouchedBy(party[0]) == -1);
    REQUIRE(chests.obstacles().size() == 1);

    // The chest of gold pays its opener; with two players the trapped one blows up.
    party[0].position = Vec3{21.9f, 0.0f, 0.0f};
    chests.update(1.0f / 30.0f, party);
    events = chests.update(1.0f / 30.0f, party);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].gold == 200);
    REQUIRE(events[0].contents == -1);
    chests.setPlayerCount(2);
    party[0].position = Vec3{41.9f, 0.0f, 0.0f};
    chests.update(1.0f / 30.0f, party);
    events = chests.update(1.0f / 30.0f, party);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].explodes);
    REQUIRE(events[0].contents == -1);
}

TEST_CASE("a gate bars the way until a key is spent on it", "[game][world][fixtures]") {
    Fixture f("fixtures-gates");
    LockedGates gates;
    REQUIRE(gates.bind(f.device, f.layout, f.items, nullptr));
    REQUIRE(gates.size() == 1);
    gates.setPlayerCount(1);
    REQUIRE(gates.obstacles().size() == 1);
    // Turned a quarter, it lies along z: someone beside it on x is against it.
    std::array<ChestVisitor, 1> party{ChestVisitor{Vec3{81.8f, 0.0f, 2.0f}, 0.75f, 0}};
    std::vector<GateEvent> events = gates.update(2, 1.0f / 30.0f, party);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].kind == GateEvent::Kind::Refused);
    party[0].keys = 2;
    events = gates.update(2, 1.0f / 30.0f, party);
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].kind == GateEvent::Kind::Unlocked);
    REQUIRE(gates.gate(0).state != LockedGates::kShut);
    for (int i = 0; i < 40; ++i) {
        REQUIRE(gates.update(2, 1.0f / 30.0f, party).empty());
    }
    REQUIRE(gates.gate(0).state == LockedGates::kOpen);
    REQUIRE(gates.obstacles().empty());
}

TEST_CASE("a trap rests, comes out to hurt whoever is in it, and rests again",
          "[game][world][fixtures]") {
    Fixture f("fixtures-traps");
    Traps traps;
    REQUIRE(traps.bind(f.device, f.layout, f.items, nullptr, 7));
    REQUIRE(traps.size() == 1);
    traps.setPlayerCount(1);
    REQUIRE(traps.trap(0).damage == 20.0f);
    REQUIRE_FALSE(traps.armed(0));
    // Its rest is somewhere from forty to a hundred and twenty ticks.
    REQUIRE(traps.trap(0).ticksLeft >= 40);
    REQUIRE(traps.trap(0).ticksLeft < 120);
    const std::array<TrapVictim, 2> party{TrapVictim{Vec3{100.0f, 0.0f, 0.0f}, 0.75f},
                                          TrapVictim{Vec3{120.0f, 0.0f, 0.0f}, 0.75f}};
    int hits = 0;
    int armedFrames = 0;
    bool restedAgain = false;
    for (int i = 0; i < 400; ++i) {
        const std::vector<TrapHit> caught = traps.update(2, 1.0f / 30.0f, party);
        for (const TrapHit& hit : caught) {
            REQUIRE(hit.victim == 0); // the one standing in it
            REQUIRE(hit.damage == 20.0f);
            REQUIRE(traps.armed(0));
            ++hits;
        }
        armedFrames += traps.armed(0) ? 1 : 0;
        restedAgain = restedAgain || (armedFrames > 0 && !traps.armed(0));
    }
    REQUIRE(hits >= 1);
    REQUIRE(armedFrames > 0);
    REQUIRE(restedAgain);
    REQUIRE(hits <= armedFrames); // no oftener than its gap allows
}

TEST_CASE("a level scales how fast its traps cycle and how much they hurt",
          "[game][world][fixtures]") {
    Fixture f("fixtures-trap-scales");
    Traps traps;
    REQUIRE(traps.bind(f.device, f.layout, f.items, nullptr, 7, 2.0f, 0.5f));
    REQUIRE(traps.trap(0).damage == 10.0f);
    // Twice the forty to a hundred and twenty ticks it rests unscaled.
    REQUIRE(traps.trap(0).ticksLeft >= 80);
    REQUIRE(traps.trap(0).ticksLeft < 240);
    REQUIRE(traps.trap(0).subtype == Traps::kSpikes);
    traps.setPlayerCount(1);
    // Caught, a victim is left alone until the sequence it was caught in has run out twice.
    const std::array<TrapVictim, 1> party{TrapVictim{Vec3{100.0f, 0.0f, 0.0f}, 0.75f}};
    std::vector<TrapHit> caught;
    for (int i = 0; i < 400 && caught.empty(); ++i) {
        caught = traps.update(2, 1.0f / 30.0f, party);
    }
    REQUIRE(caught.size() == 1);
    REQUIRE(caught[0].pierces);
    REQUIRE(caught[0].subtype == Traps::kSpikes);
    REQUIRE(caught[0].position.x == 100.0f);
    REQUIRE(traps.update(2, 1.0f / 30.0f, party).empty());
}

TEST_CASE("barrels stand in the way until blows break them, each after its kind",
          "[game][world][fixtures]") {
    Fixture f("fixtures-barrels");
    Breakables barrels;
    REQUIRE(barrels.bind(f.device, f.layout, f.items, nullptr));
    REQUIRE(barrels.size() == 4); // one that holds a key, one that blows up, one of gas, one more
    barrels.setPlayerCount(1);
    REQUIRE(barrels.barrel(0).kind == BreakableStrike::Kind::Holding);
    REQUIRE(barrels.barrel(1).kind == BreakableStrike::Kind::Exploding);
    REQUIRE(barrels.barrel(2).kind == BreakableStrike::Kind::Poison);
    REQUIRE(barrels.barrel(3).kind == BreakableStrike::Kind::Plain);
    REQUIRE_FALSE(barrels.standing(3)); // for three players
    REQUIRE(barrels.obstacles().size() == 3);
    REQUIRE(barrels.within(Vec3{201.0f, 0.0f, 0.0f}, 3.0f) == std::vector<usize>{1, 2});
    REQUIRE(barrels.within(Vec3{201.0f, 40.0f, 0.0f}, 3.0f).empty());

    // A missile's path meets the nearer barrel first, and none when it flies over.
    REQUIRE(barrels.struckBy(Vec3{210.0f, 1.0f, 0.0f}, Vec3{190.0f, 1.0f, 0.0f}, 0.5f) ==
            std::optional<usize>{2});
    REQUIRE(barrels.struckBy(Vec3{190.0f, 1.0f, 0.0f}, Vec3{210.0f, 1.0f, 0.0f}, 0.5f) ==
            std::optional<usize>{1});
    REQUIRE_FALSE(barrels.struckBy(Vec3{210.0f, 9.0f, 0.0f}, Vec3{190.0f, 9.0f, 0.0f}, 0.5f)
                      .has_value());

    // Five hit points under an armour of one: a blow of three takes two, a feeble one one.
    std::optional<BreakableStrike> blow = barrels.strike(0, 3.0f);
    REQUIRE(blow.has_value());
    REQUIRE_FALSE(blow->broken);
    REQUIRE(barrels.barrel(0).health == 3);
    blow = barrels.strike(0, 0.5f);
    REQUIRE(barrels.barrel(0).health == 2);
    blow = barrels.strike(0, 30.0f);
    REQUIRE(blow->broken);
    REQUIRE(blow->kind == BreakableStrike::Kind::Holding);
    REQUIRE(blow->contents == 1); // the key it held
    REQUIRE(blow->position.x == 60.0f);
    REQUIRE_FALSE(barrels.standing(0));
    REQUIRE(barrels.obstacles().size() == 2);
    REQUIRE_FALSE(barrels.strike(0, 30.0f).has_value()); // broken, there is nothing to strike
    REQUIRE_FALSE(barrels.strike(3, 30.0f).has_value()); // nor one that is not there
    // Its staves stay lying; one that blew up leaves nothing.
    blow = barrels.strike(1, 30.0f);
    REQUIRE(blow->kind == BreakableStrike::Kind::Exploding);
    REQUIRE(blow->contents == -1);
    barrels.update(1.0f);
    REQUIRE(barrels.barrel(0).state == Breakables::kBroken);
    REQUIRE_FALSE(barrels.barrel(0).gone);
    REQUIRE(barrels.barrel(1).gone);
}

} // namespace
