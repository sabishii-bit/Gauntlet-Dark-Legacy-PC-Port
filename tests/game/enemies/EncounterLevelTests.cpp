#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "game/enemies/EncounterLevel.h"
#include "game/enemies/ImportedOpponentCues.h"
#include "game/enemies/OpponentCues.h"

namespace {
using namespace gdl::game;

TEST_CASE("the level's answer to a boss waking and a legend spent is an interface the cue "
          "router also gives",
          "[game][level-opponents][encounter-level]") {
    STATIC_CHECK(std::is_abstract_v<EncounterLevel>);
    STATIC_CHECK(std::has_virtual_destructor_v<EncounterLevel>);
    STATIC_CHECK(std::is_base_of_v<EncounterLevel, ImportedOpponentCues>);
    STATIC_CHECK(std::is_base_of_v<OpponentCues, ImportedOpponentCues>);
    STATIC_CHECK_FALSE(std::is_base_of_v<OpponentCues, EncounterLevel>);
}
} // namespace
