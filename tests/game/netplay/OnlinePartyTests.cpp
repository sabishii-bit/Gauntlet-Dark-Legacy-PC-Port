#include <catch2/catch_test_macros.hpp>

#include "TestSupport.h"
#include "game/app/Scenario.h"
#include "game/netplay/OnlinePair.h"
#include "game/screens/MatchInputs.h"
#include "game/screens/OnlineParty.h"

namespace {
using namespace gdl;
using namespace gdl::game;
TEST_CASE("the playable G1 harness scenario passes online character admission",
          "[netplay][online-party]") {
    const auto scenario =
        Scenario::load(test::dataDirectory().parent_path() / "tests/scenarios/netplay-g1.json");
    test::OnlinePair pair({}, false);
    OnlineParty selection;
    REQUIRE(selection.select(pair.hostSession, scenario.partyMembers()));
}
PartyMember selected(s32 device, const std::string& name, usize slot) {
    PartyMember member;
    member.player = device;
    member.slot = slot;
    member.save.name = name;
    member.save.character = 3;
    member.save.color = 2;
    member.save.gold = 1234;
    member.save.progress().experience = levelExperience(60);
    member.save.classes[0].experience = levelExperience(42);
    member.save.classUnlock = 42;
    member.save.moviesSeen = {"private/history"};
    return member;
}
TEST_CASE("online party keeps physical devices and save slots local while remapping room seats",
          "[netplay][online-party]") {
    test::OnlinePair pair({}, false);
    OnlineParty host;
    OnlineParty guest;
    const auto local = selected(3, "HOST", 27);
    const auto remote = selected(2, "GUEST", 4);
    REQUIRE(host.select(pair.hostSession, std::array{local}));
    REQUIRE(guest.select(pair.guestSession, std::array{remote}));
    CHECK_FALSE(host.members(pair.hostSession));
    CHECK_FALSE(host.inputs(pair.hostSession, {}));
    pair.start();
    CHECK(host.device(pair.hostSession, 0) == 3);
    CHECK_FALSE(host.device(pair.hostSession, 1));
    CHECK(guest.device(pair.guestSession, 1) == 2);
    CHECK_FALSE(guest.device(pair.hostSession, 1));
    CHECK_FALSE(guest.device(pair.guestSession, 4));
    const auto hostMembers = host.members(pair.hostSession);
    const auto guestMembers = guest.members(pair.guestSession);
    REQUIRE(hostMembers);
    REQUIRE(guestMembers);
    REQUIRE(hostMembers->size() == 2);
    REQUIRE(guestMembers->size() == 2);
    CHECK((*hostMembers)[0].player == 0);
    CHECK((*hostMembers)[1].player == 1);
    CHECK((*hostMembers)[0].slot == local.slot);
    CHECK_FALSE((*hostMembers)[1].slot);
    CHECK_FALSE((*guestMembers)[0].slot);
    CHECK((*guestMembers)[1].slot == remote.slot);
    CHECK((*hostMembers)[0].save.toJson() == local.save.toJson());
    CHECK((*guestMembers)[1].save.toJson() == remote.save.toJson());
    CHECK((*hostMembers)[1].save.classes[0].experience == 0);
    CHECK((*hostMembers)[1].save.classUnlock == 0);
    CHECK((*hostMembers)[1].save.moviesSeen.empty());
    CHECK((*hostMembers)[1].save.experience() == remote.save.experience());
    CHECK_FALSE(guest.members(pair.hostSession));
    pair.prepare();
    pair.ready();
    SessionInputs::Frame devices;
    devices[0].attack = true; // Not one of this guest's selected physical devices.
    devices[2].move = {{1, 0}, 1};
    const auto compact = guest.inputs(pair.guestSession, devices);
    REQUIRE(compact);
    CHECK_FALSE((*compact)[0].attack);
    CHECK((*compact)[0].move.magnitude == 1);
    CHECK((*compact)[1].move.magnitude == 0);
    REQUIRE(MatchInputs::sample(pair.guest, *compact));
    pair.pump();
    for (usize tick = 0; tick <= pair.host.context().inputLead; ++tick) {
        REQUIRE(MatchInputs::sample(pair.host, {}));
        const auto inputs = MatchInputs::advance(pair.host);
        REQUIRE(inputs);
        CHECK_FALSE((*inputs)[0].attack);
        CHECK((*inputs)[0].move.magnitude == 0);
        CHECK_FALSE((*inputs)[1].attack);
        if (tick == pair.host.context().inputLead) {
            CHECK((*inputs)[1].move.magnitude == 1);
        }
    }
    pair.guestSession.leave();
    CHECK_FALSE(guest.members(pair.guestSession));
    CHECK_FALSE(guest.inputs(pair.guestSession, devices));
}
TEST_CASE("failed online local selection does not discard the last valid choices",
          "[netplay][online-party]") {
    test::OnlinePair pair({}, false);
    OnlineParty selection;
    const auto member = selected(1, "LOCAL", 2);
    REQUIRE(selection.select(pair.hostSession, std::array{member}));
    CHECK_FALSE(selection.select(pair.hostSession, {}));
    auto invalid = member;
    SECTION("duplicate physical device") {
        CHECK_FALSE(selection.select(pair.hostSession, std::array{member, member}));
    }
    SECTION("duplicate disk slot") {
        invalid.player = 3;
        CHECK_FALSE(selection.select(pair.hostSession, std::array{member, invalid}));
    }
    SECTION("invalid device") {
        invalid.player = 4;
        CHECK_FALSE(selection.select(pair.hostSession, std::array{invalid}));
    }
    SECTION("invalid character") {
        invalid.save.character = -1;
        CHECK_FALSE(selection.select(pair.hostSession, std::array{invalid}));
    }
    pair.start();
    REQUIRE(selection.members(pair.hostSession));
    CHECK(selection.members(pair.hostSession)->front().save.name == "LOCAL");
    CHECK_FALSE(selection.select(pair.hostSession, std::array{member}));
    selection.clear();
    CHECK_FALSE(selection.members(pair.hostSession));
    CHECK_FALSE(selection.inputs(pair.hostSession, {}));
}
TEST_CASE("two plus two local parties compact sparse device IDs without crossing ownership",
          "[netplay][online-party]") {
    test::OnlinePair pair({}, false, 2, 2);
    OnlineParty host;
    OnlineParty guest;
    const std::array hostChoices{selected(3, "FOUR", 8), selected(1, "TWO", 3)};
    const std::array guestChoices{selected(2, "THREE", 9), selected(0, "ONE", 3)};
    REQUIRE(host.select(pair.hostSession, hostChoices));
    REQUIRE(guest.select(pair.guestSession, guestChoices));
    pair.start();
    const auto hostMembers = host.members(pair.hostSession);
    const auto guestMembers = guest.members(pair.guestSession);
    REQUIRE(hostMembers);
    REQUIRE(guestMembers);
    REQUIRE(hostMembers->size() == 4);
    CHECK((*hostMembers)[0].save.name == "TWO");
    CHECK((*hostMembers)[1].save.name == "FOUR");
    CHECK((*hostMembers)[2].save.name == "ONE");
    CHECK((*hostMembers)[3].save.name == "THREE");
    CHECK((*hostMembers)[0].slot == 3);
    CHECK((*hostMembers)[1].slot == 8);
    CHECK_FALSE((*hostMembers)[2].slot);
    CHECK_FALSE((*hostMembers)[3].slot);
    CHECK_FALSE((*guestMembers)[0].slot);
    CHECK_FALSE((*guestMembers)[1].slot);
    CHECK((*guestMembers)[2].slot == 3); // Same slot number on different machines is unrelated.
    CHECK((*guestMembers)[3].slot == 9);
    pair.prepare();
    pair.ready();
    SessionInputs::Frame devices;
    devices[0].move = {{1, 0}, 1};
    devices[2].move = {{-1, 0}, 1};
    devices[1].attack = true;
    const auto compact = guest.inputs(pair.guestSession, devices);
    REQUIRE(compact);
    REQUIRE(MatchInputs::sample(pair.guest, *compact));
    pair.pump();
    for (usize tick = 0; tick <= pair.host.context().inputLead; ++tick) {
        REQUIRE(MatchInputs::sample(pair.host, {}));
        const auto inputs = MatchInputs::advance(pair.host);
        REQUIRE(inputs);
        for (const auto& input : *inputs) {
            CHECK_FALSE(input.attack);
        }
        CHECK((*inputs)[0].move.magnitude == 0);
        CHECK((*inputs)[1].move.magnitude == 0);
        if (tick == pair.host.context().inputLead) {
            CHECK((*inputs)[2].move.direction == Vec2{1, 0});
            CHECK((*inputs)[3].move.direction == Vec2{-1, 0});
        }
    }
}
} // namespace
