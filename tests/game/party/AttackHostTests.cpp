#include <string>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/combat/HurtKind.h"
#include "game/party/AttackHost.h"
#include "game/party/ScriptedAttacks.h"
#include "game/players/PlayerImpact.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(std::is_abstract_v<AttackHost>);
static_assert(std::has_virtual_destructor_v<AttackHost>);
static_assert(!std::is_constructible_v<test::ScriptedAttacks, test::AttackScript&&>,
              "a scripted host borrows its script, so it may not be made from a temporary");

TEST_CASE("an attack host with nothing scripted takes every report",
          "[game][player-attacks][attack-host]") {
    test::ScriptedAttacks scripted;
    AttackHost& host = scripted;
    host.hurtByAttack(0, 10, HurtKind::Blow, {});
    host.meleeHit(3);
}

TEST_CASE("a scripted attack host hands each report to its script as the script stands",
          "[game][player-attacks][attack-host]") {
    test::AttackScript script;
    test::ScriptedAttacks scripted{script};
    AttackHost& host = scripted;
    std::vector<std::string> log;
    host.meleeHit(2); // nothing scripted yet
    script.hurt = [&](usize member, f32 damage, HurtKind kind, const PlayerImpact& impact) {
        CHECK(member == 1);
        CHECK(damage == 12.0f);
        CHECK(kind == HurtKind::Burn);
        CHECK(impact.flags == 8u);
        CHECK(impact.direction == Vec3{0, 0, 1});
        log.emplace_back("hurt");
    };
    script.meleeHit = [&](s32 seat) { log.push_back("melee " + std::to_string(seat)); };
    host.hurtByAttack(1, 12, HurtKind::Burn, PlayerImpact{.flags = 8, .direction = Vec3{0, 0, 1}});
    host.meleeHit(3);
    CHECK(log == std::vector<std::string>{"hurt", "melee 3"});
}
} // namespace
