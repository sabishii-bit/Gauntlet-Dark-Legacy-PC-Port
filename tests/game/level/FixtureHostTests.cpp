#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/combat/HurtKind.h"
#include "game/level/FixtureHost.h"
#include "game/level/ScriptedFixtures.h"
#include "game/players/PlayerImpact.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(std::is_abstract_v<FixtureHost>);
static_assert(std::has_virtual_destructor_v<FixtureHost>);
static_assert(!std::is_constructible_v<test::ScriptedFixtures, test::FixtureScript&&>,
              "a scripted host borrows its script, so it may not be made from a temporary");

TEST_CASE("a fixture host with nothing scripted takes every report and answers no",
          "[game][level-fixtures][fixture-host]") {
    test::ScriptedFixtures scripted;
    FixtureHost& host = scripted;
    std::vector<s32> reached{4};
    host.hurtMember(0, 10, HurtKind::Blow, true, {});
    CHECK_FALSE(host.lesson(3, 0));
    host.card(1, "GOLD");
    host.blastOpponents(Vec3{0}, 2, 5, reached, 0);
    CHECK(reached == std::vector<s32>{4});
    CHECK_FALSE(host.releaseEnemy(7, Vec3{0}, 1));
    host.shatterPotion(2, Vec3{0});
}

TEST_CASE("a scripted fixture host hands each report to its script as the script stands",
          "[game][level-fixtures][fixture-host]") {
    test::FixtureScript script;
    test::ScriptedFixtures scripted{script};
    FixtureHost& host = scripted;
    std::vector<std::string> log;
    script.hurt = [&](usize member, f32 damage, HurtKind kind, bool directed,
                      const PlayerImpact& impact) {
        CHECK(member == 1);
        CHECK(damage == 12.0f);
        CHECK(kind == HurtKind::Gas);
        CHECK(directed);
        CHECK(impact.flags == 8u);
        log.emplace_back("hurt");
    };
    script.help = [&](s32 id, usize member) {
        log.push_back("help " + std::to_string(id) + " " + std::to_string(member));
        return id == 17;
    };
    script.card = [&](s32 seat, std::string_view name) {
        log.push_back("card " + std::to_string(seat) + " " + std::string{name});
    };
    script.opponents = [&](const Vec3& position, f32 radius, f32 damage, std::vector<s32>& reached,
                           u32 flags) {
        CHECK(position == Vec3{1, 2, 3});
        CHECK(radius == 4.0f);
        CHECK(damage == 5.0f);
        CHECK(flags == 6u);
        reached.push_back(31);
        log.emplace_back("opponents");
    };
    script.releaseEnemy = [&](s32 record, const Vec3& position, s32 count) {
        CHECK(position == Vec3{7, 0, 0});
        log.push_back("release " + std::to_string(record) + " " + std::to_string(count));
        return record == 9;
    };
    script.shatterPotion = [&](s32 kind, const Vec3& position) {
        CHECK(position == Vec3{0, 1, 0});
        log.push_back("potion " + std::to_string(kind));
    };
    std::vector<s32> reached{4};
    host.hurtMember(1, 12, HurtKind::Gas, true, PlayerImpact{.flags = 8, .direction = Vec3{0}});
    CHECK(host.lesson(17, 2));
    CHECK_FALSE(host.lesson(18, 0));
    host.card(3, "GOLD");
    host.blastOpponents(Vec3{1, 2, 3}, 4, 5, reached, 6);
    CHECK(host.releaseEnemy(9, Vec3{7, 0, 0}, 2));
    CHECK_FALSE(host.releaseEnemy(8, Vec3{7, 0, 0}, 1));
    host.shatterPotion(2, Vec3{0, 1, 0});
    CHECK(reached == std::vector<s32>{4, 31});
    CHECK(log == std::vector<std::string>{"hurt", "help 17 2", "help 18 0", "card 3 GOLD",
                                          "opponents", "release 9 2", "release 8 1", "potion 2"});
}
} // namespace
