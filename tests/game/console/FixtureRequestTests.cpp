#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"

#include "game/console/FixtureRequest.h"
#include "game/level/SyntheticLayout.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Args = std::vector<std::string>;

/** What each of the fixture commands answers words it cannot read. */
constexpr std::string_view kActivateUsage =
    "activate $SELECTED     fires the switch selected, as if you stood on it\n"
    "activate switch#N      the same, by the token list switches gives\n"
    "activate <id or name>  the same, by a switch's id, its name or its target's\n"
    "The camera follows what it opens, as it does for a player.";
constexpr std::string_view kLockUsage =
    "lock $SELECTED|gate#N|chest#N|<name>  a door shut again, or a chest wanting a key again\n"
    "lock character <code>                 a character hidden again; the starting eight stay\n"
    "list characters names the codes; list switches and get info name the rest.";
constexpr std::string_view kUnlockUsage =
    "unlock $SELECTED|gate#N|chest#N|<name>  a door or chest a key opens, opened without one\n"
    "unlock character <code>                 a hidden character, for the select screen\n"
    "list characters names the codes; list switches and get info name the rest.";

TEST_CASE("lock and unlock take a character by code or a door or chest by token or name",
          "[dev-console][commands]") {
    const auto character = parseLockRequest(Args{"Character", "min"}, LockVerb::Unlock);
    REQUIRE(character.has_value());
    CHECK(character->character);
    CHECK(character->what == "min");
    const auto fixture = parseLockRequest(Args{"gate#2"}, LockVerb::Lock);
    REQUIRE(fixture.has_value());
    CHECK_FALSE(fixture->character);
    CHECK(fixture->what == "gate#2");
    for (const Args& args : {Args{}, Args{"character"}, Args{"a", "b"}, Args{"a", "b", "c"}}) {
        CAPTURE(args.size());
        const auto request = parseLockRequest(args, LockVerb::Lock);
        CHECK_FALSE(request.has_value());
        if (!request.has_value()) {
            CHECK(request.error() == lockUsage(LockVerb::Lock));
        }
    }
    CHECK(lockUsage(LockVerb::Unlock).contains("unlock character <code>"));
    CHECK(activateUsage().contains("activate switch#N"));
    // A token names one thing alone; a name every thing that answers to it, in any case.
    const std::vector<NamedEntry> entries{{"switch#0", "BRIDGEPAD", "E1BRIDGE"},
                                          {"switch#1", "BRIDGEPAD", "E1DOOR"},
                                          {"switch#2", "TRIGGER2", "E1DOOR"}};
    CHECK(entriesNamed(entries, "SWITCH#1") == std::vector<usize>{1});
    CHECK(entriesNamed(entries, "bridgepad") == std::vector<usize>{0, 1});
    CHECK(entriesNamed(entries, "e1door") == std::vector<usize>{1, 2});
    CHECK(entriesNamed(entries, "trigger2") == std::vector<usize>{2});
    CHECK(entriesNamed(entries, "switch#9").empty());
    CHECK(entriesNamed(entries, "nothing").empty());
}

TEST_CASE("a layout's switches read the same without the disc", "[dev-console][commands]") {
    const WorldLayout layout = test::syntheticLayout();
    const auto switches = switchesOf(layout);
    REQUIRE(switches.size() == 3);
    CHECK(switches[0] ==
          SwitchEntry{.index = 0, .id = 10, .name = "BRIDGEPAD", .target = "E1DOOR"});
    CHECK(switches[1] ==
          SwitchEntry{
              .index = 1, .id = 11, .name = "LIFTPAD", .target = "E1BRIDGE", .chained = true});
    CHECK(switches[2] == SwitchEntry{.index = 2, .id = 12, .name = "TARGET", .shootable = true});
    const auto named = switchEntries(switches);
    REQUIRE(named.size() == switches.size());
    CHECK(named[0] == NamedEntry{"switch#0", "BRIDGEPAD", "E1DOOR"});
    CHECK(entriesNamed(named, "liftpad") == std::vector<usize>{1});
}

TEST_CASE("lock, unlock and activate read their usage as they do today",
          "[dev-console][commands]") {
    CHECK(activateUsage() == kActivateUsage);
    CHECK(lockUsage(LockVerb::Lock) == kLockUsage);
    CHECK(lockUsage(LockVerb::Unlock) == kUnlockUsage);
}

TEST_CASE("lock and unlock refuse the words they cannot read as they do today",
          "[dev-console][commands]") {
    const auto lock = parseLockRequest(Args{"character"}, LockVerb::Lock);
    REQUIRE_FALSE(lock.has_value());
    CHECK(lock.error() == kLockUsage);
    const auto unlock = parseLockRequest(Args{"character"}, LockVerb::Unlock);
    REQUIRE_FALSE(unlock.has_value());
    CHECK(unlock.error() == kUnlockUsage);
}

} // namespace
