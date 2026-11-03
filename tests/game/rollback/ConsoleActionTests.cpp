#include <array>
#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "TestSupport.h"
#include "game/rollback/ConsoleAction.h"
#include "game/rollback/InputCommand.h"
#include "game/rollback/InputJournal.h"

namespace {
using namespace gdl;
using namespace gdl::game;

/** The console's commands that never run in the shared game, each with words after it. */
constexpr std::array<std::string_view, 10> kLocalLines{
    "help me",   "console_permissions 1", "debug_info on", "hidehud now",  "freecam on",
    "list maps", "activate switch#0",     "unlock gate#0", "lock chest#1", "party_count 2"};

bool valid(std::string line, u8 seat = 0) {
    return ConsoleAction{seat, std::move(line)}.valid();
}

TEST_CASE("the shared console admits exactly its eight verbs, in any case",
          "[netplay][console-input][online-console]") {
    for (const std::string_view line :
         {"get", "goto", "unlock_tower", "suicide", "god", "notarget", "noclip", "spawn", "GOD",
          "  god", "NoClip", "Get Gold 5", "get information", "spawn grunt 2"}) {
        CAPTURE(line);
        CHECK(valid(std::string(line)));
    }
    for (const std::string_view line :
         {"", "   ", "get info", "GET INFO enemy#1", "get  info", "getinfo", "noclipx", "god\tx"}) {
        CAPTURE(line);
        CHECK_FALSE(valid(std::string(line)));
    }
    for (const std::string_view line : kLocalLines) {
        CAPTURE(line);
        CHECK_FALSE(valid(std::string(line)));
    }
    constexpr char kDelete = 127;
    CHECK_FALSE(valid(std::string("god") + kDelete));
    std::string longest = "god";
    longest.resize(ConsoleAction::kMaxLine, ' ');
    CHECK(valid(longest));
    longest.push_back(' ');
    REQUIRE(longest.size() == 161);
    CHECK_FALSE(valid(longest));
    for (u8 seat = 0; seat < InputCommand::kSeats; ++seat) {
        CAPTURE(seat);
        CHECK(valid("god", seat));
    }
    CHECK_FALSE(valid("god", 4));
}

TEST_CASE("a console line's packet bytes stay as they are",
          "[netplay][console-input][online-console]") {
    InputCommand command;
    command.epoch = 3;
    command.tick = 7;
    command.grant = 9;
    command.seat = 1;
    command.console = ConsoleAction{1, "spawn grunt 2"};
    command.consoleFlight = {0.5f, -1, 0};
    const auto input = InputPacket::encode(std::array{command});
    REQUIRE(input);
    constexpr std::string_view kInput = "47444c49"                    // magic
                                        "0600"                        // version
                                        "01"                          // commands
                                        "00"                          // zero
                                        "0300000000000000"            // epoch
                                        "0700000000000000"            // tick
                                        "09000000"                    // grant
                                        "01000000"                    // seat, aim, hint, revision
                                        "000000000000000000000000"    // direction, magnitude
                                        "000000000000000000000000"    // aim
                                        "0000000000000000"            // held, pressed
                                        "408100"                      // flight
                                        "0d"                          // console length
                                        "01"                          // console seat
                                        "737061776e206772756e742032"; // "spawn grunt 2"
    CHECK(test::hexBytes(*input) == kInput);
    const auto decoded = InputPacket::decode(*input);
    REQUIRE(decoded);
    REQUIRE(decoded->size() == 1);
    CHECK(decoded->front().console == command.console);

    JournalPacket::Chunk chunk;
    chunk.epoch = 3;
    chunk.firstTick = 7;
    chunk.count = 1;
    chunk.seats = 0b0010;
    chunk.last = true;
    chunk.commands = {command};
    const auto journal = JournalPacket::encode(chunk);
    REQUIRE(journal);
    constexpr std::string_view kJournal = "47444c4a"                   // magic
                                          "02010200"                   // version, last, seats
                                          "0300000000000000"           // epoch
                                          "0700000000000000"           // first tick
                                          "0100"                       // count
                                          "0000"                       // zero
                                          "09000000"                   // grant
                                          "000000000000000000000000"   // direction, magnitude
                                          "00000000000000000000000000" // aim flag, aim
                                          "0000000000000000"           // held, pressed
                                          "0000"                       // hint topic, revision
                                          "408100"                     // flight
                                          "0d"                         // console length
                                          "01"                         // console seat
                                          "737061776e206772756e742032" // "spawn grunt 2"
                                          "0100";                      // run
    CHECK(test::hexBytes(*journal) == kJournal);
    const auto chunkBack = JournalPacket::decode(*journal);
    REQUIRE(chunkBack);
    REQUIRE(chunkBack->commands.size() == 1);
    CHECK(chunkBack->commands.front().console == command.console);
}

} // namespace
