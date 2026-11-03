#include <string>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/bosses/Bosses.h"
#include "game/bosses/LegendItems.h"
#include "game/combat/CombatArenaTarget.h"
#include "game/critters/CombatEvents.h"
#include "game/enemies/BossEncounterHost.h"
#include "game/enemies/ScriptedOpponents.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(std::is_abstract_v<BossEncounterHost>);
static_assert(std::has_virtual_destructor_v<BossEncounterHost>);
static_assert(std::is_base_of_v<BossEncounterHost, test::ScriptedOpponents>);

TEST_CASE("a boss encounter host with nothing scripted takes every step and has no arena",
          "[game][level-opponents][boss-encounter-host]") {
    test::ScriptedOpponents scripted;
    BossEncounterHost& encounter = scripted;
    encounter.legend({.cue = LegendCue::Brandished, .player = 0, .realm = 1});
    encounter.advanceLegend(0.1f);
    encounter.spew({});
    encounter.fallen(Vec3{0});
    encounter.advanceVictory(2, 1.0f / 30);
    CHECK(encounter.arenaAnchors().empty());
    CHECK(encounter.arenaTargets().empty());
    encounter.activateArena({.index = 2, .delay = 0.5f});
}

TEST_CASE("a scripted boss encounter host asks its script as the script stands",
          "[game][level-opponents][boss-encounter-host]") {
    test::OpponentScript script;
    test::ScriptedOpponents scripted{script};
    BossEncounterHost& encounter = scripted;
    std::vector<std::string> log;
    CHECK(encounter.arenaAnchors().empty()); // nothing scripted yet
    script.legend = [&](const LegendEvent& event) {
        CHECK(event.cue == LegendCue::Brandished);
        log.push_back("legend " + std::to_string(event.player) + " " + std::to_string(event.realm));
    };
    script.advanceLegend = [&](f32 seconds) {
        CHECK(seconds == 0.1f);
        log.emplace_back("advanceLegend");
    };
    script.spew = [&](const CombatSpew& spew) {
        log.push_back("spew " + std::to_string(spew.critter));
    };
    script.fallen = [&](const Vec3& where) {
        CHECK(where == Vec3{1, 2, 3});
        log.emplace_back("fallen");
    };
    script.advanceVictory = [&](s32 ticks, f32 seconds) {
        CHECK(seconds == 0.5f);
        log.push_back("victory " + std::to_string(ticks));
    };
    script.arenaAnchors = [&] {
        log.emplace_back("anchors");
        return std::vector<Mat4>{Mat4{1}, Mat4{2}};
    };
    script.arenaTargets = [&] {
        log.emplace_back("targets");
        return std::vector<CombatArenaTarget>{{.index = 3, .placement = Mat4{1}, .active = true}};
    };
    script.activateArena = [&](const CombatArenaActivation& activation) {
        CHECK(activation.delay == 0.25f);
        log.push_back("activate " + std::to_string(activation.index));
    };
    encounter.legend({.cue = LegendCue::Brandished, .player = 2, .realm = 5});
    encounter.advanceLegend(0.1f);
    encounter.spew({.critter = 7});
    encounter.fallen(Vec3{1, 2, 3});
    encounter.advanceVictory(15, 0.5f);
    CHECK(encounter.arenaAnchors() == std::vector<Mat4>{Mat4{1}, Mat4{2}});
    const auto targets = encounter.arenaTargets();
    REQUIRE(targets.size() == 1);
    CHECK(targets[0].index == 3);
    CHECK(targets[0].active);
    encounter.activateArena({.index = 4, .delay = 0.25f});
    CHECK(log == std::vector<std::string>{"legend 2 5", "advanceLegend", "spew 7", "fallen",
                                          "victory 15", "anchors", "targets", "activate 4"});
}
} // namespace
