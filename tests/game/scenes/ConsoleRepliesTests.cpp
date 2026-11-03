#include <array>
#include <optional>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "engine/math/Math.h"

#include "game/scenes/ConsoleReplies.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Play = PlayScene::ConsolePlay;

TEST_CASE("offline play and the shared game each keep their own console replies",
          "[dev-console][online-console]") {
    // The toggles.
    CHECK(cheatReply(Play::Offline, "god", true) == "God mode on");
    CHECK(cheatReply(Play::Offline, "god", false) == "God mode off");
    CHECK(cheatReply(Play::Offline, "notarget", true) == "The enemies no longer see you");
    CHECK(cheatReply(Play::Offline, "notarget", false) == "The enemies see you again");
    CHECK(cheatReply(Play::Offline, "noclip", true) ==
          "Noclip on: the movement keys or left stick fly you as the camera looks, Q and E or the "
          "right stick up and down, Shift slows");
    CHECK(cheatReply(Play::Offline, "noclip", false) == "Noclip off");
    CHECK(cheatReply(Play::Shared, "god", true) == "god on");
    CHECK(cheatReply(Play::Shared, "god", false) == "god off");
    CHECK(cheatReply(Play::Shared, "notarget", true) == "notarget on");
    CHECK(cheatReply(Play::Shared, "notarget", false) == "notarget off");
    CHECK(cheatReply(Play::Shared, "noclip", true) == "noclip on");
    CHECK(cheatReply(Play::Shared, "noclip", false) == "noclip off");

    // suicide and unlock_tower.
    CHECK(fallenReply(Play::Offline, 0) == "Nobody was standing.");
    CHECK(fallenReply(Play::Offline, 1) == "1 fallen");
    CHECK(fallenReply(Play::Offline, 2) == "2 fallen");
    CHECK(fallenReply(Play::Shared, 0) == "0 fallen");
    CHECK(fallenReply(Play::Shared, 2) == "2 fallen");
    CHECK(towerOpenedReply(Play::Offline, 0) == "Nobody is in to open it for.");
    CHECK(towerOpenedReply(Play::Offline, 1) == "The tower stands open to 1 character.");
    CHECK(towerOpenedReply(Play::Offline, 2) == "The tower stands open to 2 characters.");
    CHECK(towerOpenedReply(Play::Shared, 0) == "The tower stands open to the party.");
    CHECK(towerOpenedReply(Play::Shared, 2) == "The tower stands open to the party.");

    // goto, and a map no level is.
    const Vec3 at{1.0f, -2.25f, 3.5f};
    CHECK(movedReply(Play::Offline, at) == "Moved to 1.0 -2.2 3.5");
    CHECK(movedReply(Play::Offline, std::nullopt) == "Nobody is standing to move.");
    CHECK(movedReply(Play::Shared, at) == "Moved the party.");
    CHECK(movedReply(Play::Shared, std::nullopt) == "Nobody is standing.");
    CHECK(unknownMapReply(Play::Offline, "Z9") == "No map named Z9; list maps names them");
    CHECK(unknownMapReply(Play::Shared, "Z9") == "No map named Z9");

    // spawn.
    CHECK(spawnedEnemiesReply(Play::Offline, 0, "GRU", 2) ==
          "No room for GRU here, or its kind is missing");
    CHECK(spawnedEnemiesReply(Play::Offline, 3, "GRU", 2) == "Spawned 3 GRU at tier 2");
    CHECK(spawnedEnemiesReply(Play::Shared, 0, "GRU", 2) == "Spawned 0 GRU at tier 2");
    CHECK(spawnedEnemiesReply(Play::Shared, 3, "GRU", 2) == "Spawned 3 GRU at tier 2");
    CHECK(critterReply(Play::Offline, true, "golem", "") == "Spawned a golem");
    CHECK(critterReply(Play::Offline, true, "gargoyle", "GAR_LION") == "Spawned a GAR_LION");
    CHECK(critterReply(Play::Offline, false, "golem", "") ==
          "No room for a golem here, or its kind is missing");
    CHECK(critterReply(Play::Offline, false, "gargoyle", "GAR_LION") ==
          "No room for a GAR_LION here, or its kind is missing");
    CHECK(critterReply(Play::Shared, true, "golem", "") == "Spawned a golem");
    CHECK(critterReply(Play::Shared, true, "gargoyle", "GAR_LION") == "Spawned a gargoyle");
    CHECK(critterReply(Play::Shared, false, "golem", "") == "No room or artwork for golem");
    CHECK(critterReply(Play::Shared, false, "gargoyle", "GAR_LION") ==
          "No room or artwork for gargoyle");
    CHECK(broodRealmRefusal(Play::Offline) ==
          "A mixed brood is the temple's (E) or the underworld's (F); name a map of either");
    CHECK(broodRealmRefusal(Play::Shared) ==
          "A mixed brood needs a Temple (E) or Underworld (F) map.");
    CHECK(pickupWithMapRefusal(Play::Offline) ==
          "Pickups come from the level in play alone; name one without a map");
    CHECK(pickupWithMapRefusal(Play::Shared) ==
          "Pickups come from the current level; omit the map.");
    CHECK(pickupMissingReply(Play::Offline, "NOPE") ==
          "No pickup record named NOPE here; list items names them");
    CHECK(pickupMissingReply(Play::Shared, "NOPE") == "No pickup named NOPE");
    CHECK(objectMissingReply(Play::Offline, "NOPE", "G1") ==
          "Nothing named NOPE in G1; list objects G1 names what there is");
    CHECK(objectMissingReply(Play::Shared, "NOPE", "G1") == "No object named NOPE in G1");
    CHECK(thingMissingReply(Play::Offline, "NOPE", "G1") ==
          "Nothing named NOPE here; list items, objects, enemies or critters name what there is");
    CHECK(thingMissingReply(Play::Shared, "NOPE", "G1") == "No object named NOPE in G1");

    // What offline play says before a spawn's reply, and the shared game leaves unsaid.
    CHECK(voicesNote(Play::Offline, "E1") == "(E1 lends no voices)");
    CHECK(voicesNote(Play::Shared, "E1") == std::nullopt);
    CHECK(notUnpackedNote(Play::Offline, "E1") == "E1 is not unpacked");
    CHECK(notUnpackedNote(Play::Shared, "E1") == std::nullopt);
}

TEST_CASE("offline play puts a spawn's notes before its reply, a line each",
          "[dev-console][online-console]") {
    constexpr std::string_view kStood = "Stood a tier 1 SPECIAL generator";
    const std::array offline{voicesNote(Play::Offline, "E1"), notUnpackedNote(Play::Offline, "E1")};
    CHECK(afterNotes(offline, kStood) ==
          "(E1 lends no voices)\nE1 is not unpacked\nStood a tier 1 SPECIAL generator");
    const std::array voices{voicesNote(Play::Offline, "E1")};
    CHECK(afterNotes(voices, "Spawned 1 GRU at tier 1") ==
          "(E1 lends no voices)\nSpawned 1 GRU at tier 1");
    const std::array unpacked{std::optional<std::string>{}, notUnpackedNote(Play::Offline, "E1")};
    CHECK(afterNotes(unpacked, kStood) == "E1 is not unpacked\nStood a tier 1 SPECIAL generator");
    const std::array shared{voicesNote(Play::Shared, "E1"), notUnpackedNote(Play::Shared, "E1")};
    CHECK(afterNotes(shared, kStood) == kStood);
}

} // namespace
