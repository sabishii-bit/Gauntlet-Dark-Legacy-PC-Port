#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "game/console/PartyCountRequest.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Args = std::vector<std::string>;
using Mode = PartyCountRequest::Mode;

/** What party_count answers words it cannot read. */
constexpr std::string_view kPartyCountUsage =
    "party_count <1-4>  sets the level up for a party that size\n"
    "party_count off    follows the players present again\n"
    "party_count        says which it is";

TEST_CASE("party_count takes a count from one to four, or off", "[dev-console][commands]") {
    const auto three = parsePartyCount(Args{"3"});
    REQUIRE(three.has_value());
    CHECK(three->mode == Mode::Fixed);
    CHECK(three->players == 3);
    const auto off = parsePartyCount(Args{"OFF"});
    REQUIRE(off.has_value());
    CHECK(off->mode == Mode::Players);
    const auto automatic = parsePartyCount(Args{"auto"});
    REQUIRE(automatic.has_value());
    CHECK(automatic->mode == Mode::Players);
    for (const Args& args : {Args{}, Args{"0"}, Args{"5"}, Args{"two"}, Args{"2", "3"}}) {
        CAPTURE(args.size());
        const auto count = parsePartyCount(args);
        CHECK_FALSE(count.has_value());
        if (!count.has_value()) {
            CHECK(count.error() == partyCountUsage());
        }
    }
}

TEST_CASE("party_count's usage reads as it does today", "[dev-console][commands]") {
    CHECK(partyCountUsage() == kPartyCountUsage);
}

TEST_CASE("party_count refuses the words it cannot read as it does today",
          "[dev-console][commands]") {
    for (const Args& words : {Args{}, Args{"5"}}) {
        CAPTURE(words);
        const auto count = parsePartyCount(words);
        CHECK_FALSE(count.has_value());
        if (!count.has_value()) {
            CHECK(count.error() == kPartyCountUsage);
        }
    }
}

} // namespace
