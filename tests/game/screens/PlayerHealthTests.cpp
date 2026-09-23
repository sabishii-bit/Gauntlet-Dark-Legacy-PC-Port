#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "game/screens/PlayerHealth.h"
namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

struct Fixture {
    PlayerHealth health;
    PlayerRuntime player;
    std::vector<std::string> sounds;
    std::vector<std::string> cries;
    std::vector<std::string> named;
    PlayerHealth::Events events{
        .block = [](f32, f32) { FAIL("No figure means no guard presentation"); },
        .sound = [this](std::string_view cue) { sounds.emplace_back(cue); },
        .cry = [this](std::string_view cue) { cries.emplace_back(cue); },
        .named = [this](std::string_view cue) { named.emplace_back(cue); }};
    Fixture() {
        player.actor.spawn(3, {}, nullptr, Vec3{0}, 0);
        player.actor.save().progress().health = 1000;
    }
    void hit(f32 damage, HurtKind kind = HurtKind::Blow, bool tower = false, f32 scale = 1) {
        health.hurt(player, damage, kind, false, tower, scale, events);
    }
};

TEST_CASE("player health respects tower immunity and scales only substantial damage",
          "[game][screens][player-health]") {
    Fixture f;
    f.hit(500, HurtKind::Blow, true);
    f.hit(0);
    f.hit(-10);
    REQUIRE(f.player.actor.save().health() == 1000);
    REQUIRE(f.sounds.empty());
    f.hit(1, HurtKind::Blow, false, 4);
    REQUIRE(f.player.actor.save().health() == 999);
    f.hit(2, HurtKind::Blow, false, 4);
    REQUIRE(f.player.actor.save().health() == 991);
    REQUIRE(f.sounds == std::vector<std::string>{"S_PLYRDMG"});
}

TEST_CASE("player death keeps the save sentinel and only emits cues once",
          "[game][screens][player-health]") {
    Fixture f;
    f.player.turbo.add(80);
    f.hit(1000);
    REQUIRE(f.player.actor.save().health() == 1);
    REQUIRE(f.player.life == PlayerLife::Dying);
    REQUIRE(f.player.turbo.held() == 0);
    f.hit(1);
    REQUIRE(f.sounds == std::vector<std::string>{"S_PLAYERDIES"});
    REQUIRE(f.cries == std::vector<std::string>{"DIE2"});
}

TEST_CASE("player low health announcements take precedence and alternate across participants",
          "[game][screens][player-health]") {
    Fixture f;
    f.player.actor.save().progress().health = 151;
    f.hit(1, HurtKind::Burn);
    REQUIRE(f.named == std::vector<std::string>{"S_BADLY"});
    REQUIRE(f.cries.empty());
    f.player.actor.save().progress().health = 51;
    f.hit(1, HurtKind::Pierce);
    PlayerRuntime other;
    other.actor.spawn(1, {}, nullptr, Vec3{0}, 0);
    other.actor.save().progress().health = 51;
    f.health.hurt(other, 1, HurtKind::Gas, false, false, 1, f.events);
    REQUIRE(f.named == std::vector<std::string>{"S_BADLY", "S_LIFEFORCE", "S_ABOUT"});
    REQUIRE(f.cries.empty());
}

TEST_CASE("player pain accumulates while burns pierces and gas retain their own cues",
          "[game][screens][player-health]") {
    Fixture f;
    f.hit(10);
    f.hit(10);
    REQUIRE(f.cries.empty());
    REQUIRE(f.player.hitSoundGap == 30);
    f.hit(15);
    REQUIRE(f.cries.size() == 1);
    REQUIRE(f.cries.back().starts_with("PAIN"));
    REQUIRE(f.player.painOwed == Approx(5));
    f.hit(61);
    REQUIRE(f.player.painOwed == 0);
    REQUIRE(f.cries.size() == 2);
    f.hit(1, HurtKind::Burn);
    f.hit(1, HurtKind::Pierce);
    f.hit(1, HurtKind::Gas);
    REQUIRE(f.cries.size() == 5);
    REQUIRE(f.cries[3] == "DIE1");
    REQUIRE(f.cries[4] == "POISON");
    REQUIRE(f.sounds.size() == 1);
}
} // namespace
