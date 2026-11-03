#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/enemies/OpponentCues.h"
#include "game/enemies/OpponentPool.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(std::is_abstract_v<OpponentCues>);
static_assert(std::has_virtual_destructor_v<OpponentCues>);
static_assert(std::is_aggregate_v<OpponentCues::Shot>);

TEST_CASE("a cue's owner is the native voice or the imported level's, in a byte",
          "[game][level-opponents][opponent-cues]") {
    STATIC_CHECK(std::is_same_v<std::underlying_type_t<CueOwner>, u8>);
    STATIC_CHECK(CueOwner{} == CueOwner::Native);
    STATIC_CHECK(CueOwner::Native != CueOwner::Imported);
}

TEST_CASE("a shot's sound left unset has no lifetime or effect row and is a great one's",
          "[game][level-opponents][opponent-cues]") {
    const OpponentCues::Shot shot;
    CHECK(shot.name.empty());
    CHECK(shot.position == Vec3{0.0f});
    CHECK(shot.instance == 0);
    CHECK(shot.effect == -1);
    CHECK(shot.phase == 0);
    CHECK(shot.owner == CombatantPool::Critters);
}
} // namespace
