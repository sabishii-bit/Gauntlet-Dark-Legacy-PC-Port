#include <string>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/math/Math.h"

#include "game/enemies/ScriptedOpponents.h"
#include "game/level/CombatCover.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(std::is_abstract_v<CombatCover>);
static_assert(std::has_virtual_destructor_v<CombatCover>);
static_assert(std::is_base_of_v<CombatCover, test::ScriptedOpponents>);

TEST_CASE("breath is taken wider than a wide area's blow against every cover",
          "[game][combat-cover]") {
    STATIC_CHECK(CombatCover::kBreathProbeRadius == 0.5f);
    STATIC_CHECK(CombatCover::kAreaProbeRadius == 0.1f);
}

TEST_CASE("cover with nothing scripted shelters nobody", "[game][combat-cover]") {
    const test::ScriptedOpponents scripted;
    const CombatCover& cover = scripted;
    CHECK_FALSE(cover.blocksBreath(Vec3{0}, Vec3{0, 0, 5}));
    CHECK_FALSE(cover.blocksArea(Vec3{0}, Vec3{0, 0, 15}));
}

TEST_CASE("scripted cover asks its script for breath and areas apart", "[game][combat-cover]") {
    test::OpponentScript script;
    const test::ScriptedOpponents scripted{script};
    const CombatCover& cover = scripted;
    std::vector<std::string> log;
    script.blocksBreath = [&](const Vec3& from, const Vec3& to) {
        CHECK(from == Vec3{1, 0, 0});
        log.emplace_back("breath");
        return to.z > 4.0f;
    };
    CHECK_FALSE(cover.blocksArea(Vec3{1, 0, 0}, Vec3{0, 0, 15})); // the area is not scripted yet
    CHECK(cover.blocksBreath(Vec3{1, 0, 0}, Vec3{0, 0, 5}));
    CHECK_FALSE(cover.blocksBreath(Vec3{1, 0, 0}, Vec3{0, 0, 3}));
    script.blocksArea = [&](const Vec3& from, const Vec3& to) {
        CHECK(from == Vec3{1, 0, 0});
        log.emplace_back("area");
        return to.z > 10.0f;
    };
    CHECK(cover.blocksArea(Vec3{1, 0, 0}, Vec3{0, 0, 15}));
    CHECK(log == std::vector<std::string>{"breath", "breath", "area"});
}
} // namespace
