#include <array>
#include <filesystem>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/TowerRelics.h"
#include "game/screens/TowerWelcome.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("Sumner's approach pad and the completed Temple window own different light rays",
          "[welcome][tower-relics][tower-lights][assets]") {
    const bool completeWindow = GENERATE(false, true);
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root));
    const auto& objects = world.layout().objects();
    const auto objectOf = [&](std::string_view name) {
        for (usize i = 0; i < objects.size(); ++i) {
            if (objects[i].name == name) {
                return i;
            }
        }
        FAIL("Missing tower light object: " << name);
        return usize{0};
    };
    const usize window = objectOf("L1XPLIGHTRAY01");
    const std::array beams{objectOf("L1XPLOWERLIGHTR"), objectOf("L1XPUPPERLIGHTR")};
    const usize lectern = objectOf("L1DRAWB69");
    for (const usize beam : beams) {
        REQUIRE(objects[beam].parent == static_cast<s32>(lectern));
    }
    REQUIRE(objects[window].parent != static_cast<s32>(lectern));
    Vec3 spot{0};
    bool found = false;
    for (usize i = 0; i < world.triggers().size(); ++i) {
        const auto& trigger = world.triggers().trigger(i);
        if (trigger.id == 40) {
            REQUIRE(trigger.target == static_cast<s32>(lectern));
            REQUIRE(trigger.nextId == 41); // the podium light follows the same pad
            spot = trigger.spot;
            found = true;
        }
    }
    REQUIRE(found);
    world.startTriggers({});
    TowerWelcome welcome;
    welcome.open(world, false);
    std::array<Relics, 1> collections;
    if (completeWindow) {
        collections[0].shards = 0x1FE;
    }
    TowerRelics relics;
    relics.begin(collections, MessageTable{});
    relics.bind(device, world, spot);
    const f32 windowAlpha = completeWindow ? 1.0f : 0.0f;
    CHECK(world.objectAlpha(window) == windowAlpha);
    for (const usize beam : beams) {
        CHECK(world.objectAlpha(beam) == 0);
    }
    std::array<TriggerVisitor, 1> visitors;
    visitors[0].position = spot;
    // Enter and leave twice: neither the collection nor its presentation may
    // override the pad, and the pad must never reveal or dim the window beam.
    for (s32 visit = 0; visit < 2; ++visit) {
        for (s32 frame = 0; frame < 120; ++frame) {
            world.updateTriggers(1.0f / 30.0f, visitors);
            relics.animate(1.0f / 30.0f);
            CHECK(world.objectAlpha(window) == windowAlpha);
        }
        for (const usize beam : beams) {
            CHECK(world.objectAlpha(beam) == Catch::Approx(1));
        }
        for (s32 frame = 0; frame < 120; ++frame) {
            world.updateTriggers(1.0f / 30.0f, {});
            relics.animate(1.0f / 30.0f);
            CHECK(world.objectAlpha(window) == windowAlpha);
        }
        for (const usize beam : beams) {
            CHECK(world.objectAlpha(beam) == Catch::Approx(0).margin(0.001f));
        }
    }
}

TEST_CASE("a party is new to the tower while none of its classes has experience",
          "[game][screens][welcome]") {
    CHECK_FALSE(TowerWelcome::freshParty({}));
    std::vector<PartyMember> party{PartyMember{0, CharacterSave{}},
                                   PartyMember{1, CharacterSave{}}};
    CHECK(TowerWelcome::freshParty(party));
    party[1].save.classes[3].experience = 1;
    CHECK_FALSE(TowerWelcome::freshParty(party));
}

TEST_CASE("without its scroll or its camera the welcome passes straight on",
          "[game][screens][welcome]") {
    test::FakeRenderDevice device;
    LevelWorld world;
    LevelMessages messages;
    SumnerFigure sumner;
    TowerWelcome welcome;
    welcome.open(world, true);
    CHECK(welcome.intro() == TowerWelcome::Intro::None);
    CHECK_FALSE(welcome.hold(1));
    welcome.arrived(device, messages, nullptr, world.layout(), sumner);
    CHECK(welcome.intro() == TowerWelcome::Intro::Done);
    CHECK_FALSE(welcome.camera().has_value());
    CHECK_FALSE(welcome.hold(1));
    // Once over, a second arrival starts nothing.
    welcome.arrived(device, messages, nullptr, world.layout(), sumner);
    CHECK(welcome.intro() == TowerWelcome::Intro::Done);
    welcome.clear();
    CHECK(welcome.intro() == TowerWelcome::Intro::None);
}

TEST_CASE("the welcome crystal cut holds its ticks", "[game][screens][welcome][assets]") {
    const std::filesystem::path root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root));
    SumnerFigure sumner;
    REQUIRE(sumner.load(device, world.items(), world.layout()));

    // Without the scroll, the welcome cuts to the crystals from marker 198 for its ticks.
    LevelMessages messages;
    TowerWelcome fresh;
    fresh.open(world, true);
    fresh.arrived(device, messages, nullptr, world.layout(), sumner);
    REQUIRE(fresh.intro() == TowerWelcome::Intro::Crystal);
    REQUIRE(fresh.camera().has_value());
    s32 held = 0;
    while (fresh.hold(1)) {
        ++held;
    }
    CHECK(held == TowerWelcome::kCrystalTicks);
    CHECK(fresh.intro() == TowerWelcome::Intro::Done);
}

} // namespace
