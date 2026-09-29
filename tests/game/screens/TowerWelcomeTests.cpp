#include <array>
#include <filesystem>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/TowerWelcome.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

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

TEST_CASE("Sumner's beam comes up while a player is near him and the cut holds its ticks",
          "[game][screens][welcome][unpacked]") {
    const std::filesystem::path root =
        test::unpackedOrSkip("LEVELS/LEVELL1/world.json").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root));
    SumnerFigure sumner;
    REQUIRE(sumner.load(device, world.items(), world.layout()));
    TowerWelcome welcome;
    welcome.open(world, false);
    std::array<PlayerRuntime, 1> players;
    players[0].actor.spawn(0, CharacterSave{}, nullptr,
                           sumner.position() + Vec3{TowerWelcome::kBeamRadius * 2.0f, 0, 0}, 0);
    welcome.updateBeam(world, players, sumner.position(), 60);
    CHECK(welcome.beamAlpha() == 0.0f);
    players[0].actor.place(sumner.position());
    welcome.updateBeam(world, players, sumner.position(), 90);
    CHECK(welcome.beamAlpha() == Approx(0.5f));
    welcome.updateBeam(world, players, sumner.position(), 900);
    CHECK(welcome.beamAlpha() == 1.0f);

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
