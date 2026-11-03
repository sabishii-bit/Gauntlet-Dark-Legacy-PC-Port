#include <string>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/combat/HurtKind.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/enemies/OpponentHost.h"
#include "game/enemies/ScriptedOpponents.h"
#include "game/players/PlayerImpact.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(std::is_abstract_v<OpponentHost>);
static_assert(std::has_virtual_destructor_v<OpponentHost>);
static_assert(std::is_base_of_v<OpponentHost, test::ScriptedOpponents>);
static_assert(!std::is_constructible_v<test::ScriptedOpponents, test::OpponentScript&&>,
              "a scripted host borrows its script, so it may not be made from a temporary");

TEST_CASE("an opponent host with nothing scripted takes every report and answers no",
          "[game][level-opponents][opponent-host]") {
    test::ScriptedOpponents scripted;
    OpponentHost& host = scripted;
    std::vector<s32> reached{4};
    host.hurtMember(0, 10, HurtKind::Blow, true, {});
    host.settleBlasts();
    host.blastScenery({.position = Vec3{0}, .radius = 2, .damage = 5, .flags = 0}, reached);
    CHECK(reached == std::vector<s32>{4});
    host.award(0, 3, true);
    host.destroyedGenerator(1);
    host.updateLevels();
    CHECK_FALSE(host.lesson(3, 0));
    host.shake();
}

TEST_CASE("a scripted opponent host hands each report to its script as the script stands",
          "[game][level-opponents][opponent-host]") {
    test::OpponentScript script;
    test::ScriptedOpponents scripted{script};
    OpponentHost& host = scripted;
    std::vector<std::string> log;
    script.hurt = [&](usize member, f32 damage, HurtKind kind, bool directed,
                      const PlayerImpact& impact) {
        CHECK(member == 1);
        CHECK(damage == 12.0f);
        CHECK(kind == HurtKind::Burn);
        CHECK(directed);
        CHECK(impact.flags == 8u);
        log.emplace_back("hurt");
    };
    script.settleBlasts = [&] { log.emplace_back("settle"); };
    script.blastScenery = [&](const PickupBlastReach& reach, std::vector<s32>& reached) {
        CHECK(reach.position == Vec3{1, 2, 3});
        CHECK(reach.radius == 4.0f);
        CHECK(reach.damage == 5.0f);
        CHECK(reach.flags == 6u);
        reached.push_back(31);
        log.emplace_back("scenery");
    };
    script.award = [&](s32 player, s32 amount, bool kill) {
        log.push_back("award " + std::to_string(player) + " " + std::to_string(amount) + " " +
                      (kill ? "kill" : "hit"));
    };
    script.destroyedGenerator = [&](s32 player) {
        log.push_back("generator " + std::to_string(player));
    };
    script.levels = [&] { log.emplace_back("levels"); };
    script.help = [&](s32 id, usize member) {
        log.push_back("help " + std::to_string(id) + " " + std::to_string(member));
        return id == 17;
    };
    script.shake = [&] { log.emplace_back("shake"); };
    std::vector<s32> reached{4};
    host.hurtMember(1, 12, HurtKind::Burn, true, PlayerImpact{.flags = 8, .direction = Vec3{0}});
    host.settleBlasts();
    host.blastScenery({.position = Vec3{1, 2, 3}, .radius = 4, .damage = 5, .flags = 6}, reached);
    host.award(2, 40, false);
    host.destroyedGenerator(3);
    host.updateLevels();
    CHECK(host.lesson(17, 2));
    CHECK_FALSE(host.lesson(18, 0));
    host.shake();
    CHECK(reached == std::vector<s32>{4, 31});
    CHECK(log == std::vector<std::string>{"hurt", "settle", "scenery", "award 2 40 hit",
                                          "generator 3", "levels", "help 17 2", "help 18 0",
                                          "shake"});
}
} // namespace
