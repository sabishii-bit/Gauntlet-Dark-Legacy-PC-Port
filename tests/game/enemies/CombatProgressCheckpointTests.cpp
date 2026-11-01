#include <limits>
#include <random>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "game/characters/LevelWatch.h"
#include "game/party/PlayerHealth.h"
#include "game/players/MeleeStreak.h"

namespace {
using namespace gdl;
using namespace gdl::game;

TEST_CASE(
    "health cue checkpoints preserve the existing random sequence without emitting on restore",
    "[rollback][combat-progress-checkpoint]") {
    PlayerHealth health;
    std::mt19937 reference{0x5A17u};
    std::vector<std::string> cries;
    PlayerHealth::Events events{
        .block = [](f32, f32) { FAIL("unexpected guard"); },
        .sound = [](std::string_view) {},
        .cry = [&](std::string_view cue) { cries.emplace_back(cue); },
        .named = [](std::string_view, f32) { FAIL("unexpected low-health line"); }};
    const auto burn = [&] {
        PlayerRuntime player;
        CharacterSave save;
        save.progress().health = 1000;
        player.actor.spawn(0, save, nullptr, Vec3{0}, 0);
        health.hurt(player, 1, HurtKind::Burn, false, false, 1, events);
        REQUIRE(player.actor.save().health() == 999);
    };
    for (s32 index = 0; index < 257; ++index) {
        burn();
        CHECK(cries.back() == "PAIN" + std::to_string(1 + reference() % 4));
    }
    const auto saved = health.checkpoint();
    REQUIRE(saved.valid());
    cries.clear();
    for (s32 index = 0; index < 700; ++index) {
        burn();
        CHECK(cries.back() == "PAIN" + std::to_string(1 + reference() % 4));
    }
    const auto expected = cries;
    cries.clear();
    REQUIRE(health.restore(saved));
    CHECK(cries.empty());
    CHECK(health.checkpoint() == saved);
    for (s32 index = 0; index < 700; ++index) {
        burn();
    }
    CHECK(cries == expected);
    const auto before = health.checkpoint();
    auto bad = before;
    bad.cursor = static_cast<u32>(Mt19937::state_size + 1);
    CHECK_FALSE(health.restore(bad));
    CHECK(health.checkpoint() == before);
    bad.words.fill(0);
    bad.cursor = 0;
    CHECK_FALSE(health.restore(bad));
    CHECK(health.checkpoint() == before);
}

TEST_CASE("melee praise restores its exact count and fractional quiet timer",
          "[rollback][combat-progress-checkpoint]") {
    MeleeStreak native;
    for (s32 hit = 0; hit < 15; ++hit) {
        native.record(true);
    }
    REQUIRE(native.step(1.25f).empty());
    const auto saved = native.checkpoint();
    REQUIRE(saved.valid());
    MeleeStreak replay;
    REQUIRE(replay.restore(saved));
    CHECK(replay.checkpoint() == saved);
    CHECK(native.step(0.5f).empty());
    CHECK(replay.step(0.5f).empty());
    CHECK(native.step(0.26f) == MeleeStreak::kBraveryVoice); // past the original's two seconds
    CHECK(replay.step(0.26f) == MeleeStreak::kBraveryVoice);
    CHECK(replay.checkpoint() == native.checkpoint());
    CHECK(replay.step(4).empty());
    REQUIRE(replay.restore(saved));
    auto bad = saved;
    SECTION("nonfinite timer") {
        bad.quiet = std::numeric_limits<f32>::infinity();
    }
    SECTION("negative counter") {
        bad.count = -1;
    }
    SECTION("already expired timer") {
        bad.quiet = MeleeStreak::kLongestQuiet + 1;
    }
    SECTION("inconsistent stopped timer") {
        bad.running = false;
    }
    CHECK_FALSE(replay.restore(bad));
    CHECK(replay.checkpoint() == saved);
}

TEST_CASE("level watches restore native party order and pending level changes",
          "[rollback][combat-progress-checkpoint]") {
    LevelWatch native;
    REQUIRE_FALSE(native.observe(3, 9));
    REQUIRE_FALSE(native.observe(0, 14));
    const auto saved = native.checkpoint();
    REQUIRE(saved.valid());
    REQUIRE(saved.marks.front().player == 3);
    LevelWatch replay;
    REQUIRE(replay.restore(saved));
    CHECK(replay.checkpoint() == saved);
    const auto gained = replay.observe(3, 10);
    REQUIRE(gained);
    CHECK(gained->from == 9);
    CHECK(gained->to == 10);
    CHECK(gained->milestone());
    CHECK_FALSE(replay.observe(3, 10));
    REQUIRE(replay.restore(saved));
    auto bad = saved;
    SECTION("duplicate seat") {
        bad.marks.push_back(saved.marks.front());
    }
    SECTION("invalid seat") {
        bad.marks.back().player = 4;
    }
    SECTION("invalid level") {
        bad.marks.back().level = 100;
    }
    CHECK_FALSE(replay.restore(bad));
    CHECK(replay.checkpoint() == saved);
    native.forget(3);
    REQUIRE(replay.restore(native.checkpoint()));
    CHECK_FALSE(replay.markOf(3));
    CHECK_FALSE(replay.observe(3, 10));
}
} // namespace
