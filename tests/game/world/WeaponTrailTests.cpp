#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "game/world/WeaponTrail.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

Mat4 at(f32 x) {
    return glm::translate(Mat4{1.0f}, Vec3{x, 0.0f, 0.0f});
}

TEST_CASE("a heavy swing leaves fading ghosts of the weapon, eight at most",
          "[game][world][weapon-trail]") {
    WeaponTrail trail;
    trail.step(2, at(0.0f), false);
    CHECK(trail.count() == 0);
    trail.step(2, at(0.0f), true);
    REQUIRE(trail.count() == 1);
    // Moved less than a tenth, it leaves no other.
    trail.step(2, at(0.05f), true);
    CHECK(trail.count() == 1);
    // Each copy fades 32 a tick: a 30 Hz frame of two ticks takes 64 of 255.
    const WeaponTrail::Ghost* first = nullptr;
    for (const WeaponTrail::Ghost& ghost : trail.ghosts()) {
        if (ghost.shown) {
            first = &ghost;
            break;
        }
    }
    REQUIRE(first != nullptr);
    CHECK(first->fade == 64);
    CHECK(first->alpha() == Approx(1.0f - 64.0f / 255.0f));
    // Further on, another; in four frames the first is gone.
    trail.step(2, at(1.0f), true);
    CHECK(trail.count() == 2);
    trail.step(2, at(1.0f), false);
    trail.step(2, at(1.0f), false);
    CHECK(trail.count() == 1);
    trail.step(2, at(1.0f), false);
    CHECK(trail.count() == 1);
    trail.step(2, at(1.0f), false);
    CHECK(trail.count() == 0);
    // Never more than eight: the faintest gives way.
    for (s32 i = 0; i < 12; ++i) {
        trail.step(1, at(static_cast<f32>(i)), true);
    }
    CHECK(trail.count() == WeaponTrail::kMost);
    trail.clear();
    CHECK(trail.count() == 0);
}

} // namespace
