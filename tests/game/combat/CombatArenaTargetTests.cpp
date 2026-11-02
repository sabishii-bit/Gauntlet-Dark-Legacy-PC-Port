#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"
#include "engine/math/Math.h"

#include "game/combat/CombatArenaTarget.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE("an arena target starts at the first anchor, unplaced and standing down",
          "[combat-arena-target]") {
    STATIC_CHECK(std::is_same_v<decltype(CombatArenaTarget::index), usize>);
    STATIC_CHECK(std::is_same_v<decltype(CombatArenaTarget::placement), Mat4>);
    STATIC_CHECK(std::is_same_v<decltype(CombatArenaTarget::active), bool>);
    const CombatArenaTarget target;
    CHECK(target.index == 0);
    CHECK(target.placement == Mat4{1});
    CHECK_FALSE(target.active);
    CHECK(target == CombatArenaTarget{0, Mat4{1}, false});
    CHECK_FALSE(target == CombatArenaTarget{0, Mat4{1}, true});
}

TEST_CASE("an arena target digests its index wide, its placement by columns, then its activity",
          "[combat-arena-target][native-digest]") {
    const CombatArenaTarget target{7, glm::translate(Mat4{1}, Vec3{3, 0, -8}), true};
    NativeStateDigest digest;
    digest.value(target);

    NativeStateDigest expected;
    expected.wide(7);
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            expected.real(target.placement[column][row]);
        }
    }
    expected.flag(true);
    CHECK(digest.finish() == expected.finish());

    NativeStateDigest standing;
    standing.value(CombatArenaTarget{7, target.placement, false});
    CHECK_FALSE(standing.finish() == digest.finish());
}
} // namespace
