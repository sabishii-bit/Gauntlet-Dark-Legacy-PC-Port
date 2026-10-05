#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/combat/Damage.h"
#include "game/players/PowerupEffects.h"
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
        .named = [this](std::string_view cue, f32) { named.emplace_back(cue); },
        .learnBlock = [this] { ++blockLessons; }};
    s32 blockLessons = 0;
    Fixture() {
        player.actor.spawn(3, {}, nullptr, Vec3{0}, 0);
        player.actor.save().progress().health = 1000;
    }
    void hit(f32 damage, HurtKind kind = HurtKind::Blow, bool tower = false, f32 scale = 1) {
        health.hurt(player, damage, kind, false, tower, scale, events);
    }
};

TEST_CASE("both combo participants reject damage and reactions until unlinked",
          "[player-health][attack-invulnerability]") {
    Fixture grabber;
    Fixture partner;
    ComboMove::link(grabber.player.combo, 0, partner.player.combo, 1, 0);
    const auto checkProtected = [](Fixture& f) {
        REQUIRE_FALSE(PlayerHealth::canBeDamaged(f.player));
        f.health.hurt(f.player, 10000, HurtKind::Blow, true, false, 1, f.events,
                      {PlayerImpact::kKnockDown, Vec3{1, 0, 0}});
        f.health.hurt(f.player, 30, HurtKind::Pierce, true, false, 1, f.events,
                      {PlayerImpact::kSticky});
        f.health.hurt(f.player, 0, HurtKind::QuietBlow, true, false, 1, f.events,
                      {PlayerImpact::kStun});
        f.hit(30, HurtKind::Gas);
        CHECK(f.player.actor.save().health() == 1000);
        CHECK(f.player.life == PlayerLife::Standing);
        CHECK(f.player.reaction == PlayerDeed::None);
        CHECK_FALSE(f.player.knockback.pending());
        CHECK(f.player.hitFlashTicks == 0);
        CHECK(f.player.gagSeconds == 0);
        CHECK(f.sounds.empty());
        CHECK(f.cries.empty());
        CHECK(f.blockLessons == 0);
    };
    checkProtected(grabber);
    checkProtected(partner);
    ComboMove::advance(grabber.player.combo, partner.player.combo,
                       {.act1 = true, .frame = ComboMove::kThrowFrame}, 2);
    REQUIRE(partner.player.combo.role == ComboRole::Thrown);
    checkProtected(grabber);
    checkProtected(partner);
    ComboMove::clear(grabber.player.combo);
    ComboMove::clear(partner.player.combo);
    for (Fixture* f : {&grabber, &partner}) {
        CHECK(PlayerHealth::canBeDamaged(f->player));
        f->hit(30);
        CHECK(f->player.actor.save().health() == 970);
    }
}

TEST_CASE("stun shots react without health loss and magic never harms a player",
          "[player-health][multiplayer-combat]") {
    Fixture f;
    f.health.hurt(f.player, 0, HurtKind::QuietBlow, true, false, 1, f.events,
                  {PlayerImpact::kStun | PlayerImpact::kKnockDown, Vec3{0, 0, 1}});
    CHECK(f.player.actor.save().health() == 1000);
    CHECK(f.player.reaction == PlayerDeed::Reel);
    CHECK_FALSE(f.player.knockback.pending());
    CHECK(f.sounds.empty());
    CHECK(f.cries.empty());
    f.player.reaction = PlayerDeed::None;
    f.health.hurt(f.player, 100, HurtKind::Blow, true, false, 1, f.events,
                  {Damage::kMagic | PlayerImpact::kStun});
    CHECK(f.player.actor.save().health() == 1000);
    CHECK(f.player.reaction == PlayerDeed::None);
    f.player.actor.save().progress().inventory.addPowerup(powerup::kArmor, powerup::kInvulnerable,
                                                          0, 1);
    f.health.hurt(f.player, 0, HurtKind::QuietBlow, true, false, 1, f.events,
                  {PlayerImpact::kStun});
    CHECK(f.player.reaction == PlayerDeed::None);
}

TEST_CASE("turbo animation blocks boss knockdown throughout the move but not afterward",
          "[player-health][attack-invulnerability][assets]") {
    const auto root = test::assetOrSkip("PLAYERS/WAR/ANIM/ANIM.PS2")
                          .parent_path()
                          .parent_path()
                          .parent_path()
                          .parent_path();
    test::FakeRenderDevice device;
    for (const auto deed : {PlayerDeed::TurboFull, PlayerDeed::TurboStrong}) {
        Fixture f;
        f.player.figure = PlayerFigure::load(device, root, f.player.actor.save(), false);
        REQUIRE(f.player.figure);
        auto& figure = *f.player.figure;
        figure.animate(0, 2, 1.0f / 30, deed);
        REQUIRE(figure.animator().damageProtected());
        // Empty meter after spending must not end protection ahead of the animation.
        f.player.turbo.reset();
        s32 frames = 0;
        while (figure.animator().damageProtected() && frames < 300) {
            f.health.hurt(f.player, 0, HurtKind::QuietBlow, true, false, 1, f.events,
                          {PlayerImpact::kStun});
            f.health.hurt(f.player, 150, HurtKind::Blow, true, false, 1, f.events,
                          {PlayerImpact::kKnockDown, Vec3{1, 0, 0}}, true);
            f.hit(5, HurtKind::Gas);
            CHECK(f.player.actor.save().health() == 1000);
            CHECK(f.player.reaction == PlayerDeed::None);
            CHECK_FALSE(f.player.knockback.pending());
            figure.animate(0, 2, 1.0f / 30);
            ++frames;
        }
        REQUIRE(frames > 0);
        REQUIRE_FALSE(figure.animator().damageProtected());
        CHECK(f.sounds.empty());
        CHECK(f.cries.empty());
        f.health.hurt(f.player, 150, HurtKind::Blow, true, false, 1, f.events,
                      {PlayerImpact::kKnockDown, Vec3{1, 0, 0}}, true);
        CHECK(f.player.actor.save().health() == 850);
        CHECK(f.player.reaction != PlayerDeed::None);
        CHECK(f.player.knockback.pending());
    }
}

TEST_CASE("player armor is subtracted before elemental affinity but not from gas",
          "[player-health][damage]") {
    Fixture f;
    ClassStats stats;
    stats.armorMin = 400; // two points of flat absorption
    stats.armorMax = 800;
    PlayerImpact impact{.flags = 1};
    f.health.hurt(f.player, 20, HurtKind::Blow, false, false, 1, f.events, impact, false, &stats);
    CHECK(f.player.actor.save().health() == 973); // (20 - 2) * 1.5
    auto& inventory = f.player.actor.save().progress().inventory;
    inventory.addPowerup(6, 1, 0, 60);
    f.health.hurt(f.player, 20, HurtKind::Blow, false, false, 1, f.events, impact, false, &stats);
    CHECK(f.player.actor.save().health() == 964); // (20 - 2) * 0.5
    inventory.addPowerup(6, 0x800, 0, 60);
    impact.flags = 4;
    f.health.hurt(f.player, 20, HurtKind::Blow, false, false, 1, f.events, impact, false, &stats);
    CHECK(f.player.actor.save().health() == 964); // acid immunity remains zero
    f.health.hurt(f.player, 20, HurtKind::Gas, false, false, 1, f.events, {}, false, &stats);
    CHECK(f.player.actor.save().health() == 944);
}

TEST_CASE("armor absorbs ground-hand damage without preventing its sticky reaction",
          "[player-health][lich-hands]") {
    Fixture f;
    ClassStats stats;
    stats.armorMin = 400;
    stats.armorMax = 800;
    const PlayerImpact impact{.flags = PlayerImpact::kSticky};
    f.health.hurt(f.player, 1, HurtKind::Pierce, true, false, 1, f.events, impact, true, &stats);
    CHECK(f.player.actor.save().health() == 1000);
    CHECK(f.player.reaction == PlayerDeed::Webbed);
    CHECK(f.sounds.empty());
    CHECK(f.cries.empty());
    f.player.reaction = PlayerDeed::FallBack;
    f.health.hurt(f.player, 1, HurtKind::Pierce, true, false, 1, f.events, impact, true, &stats);
    CHECK(f.player.reaction == PlayerDeed::FallBack);
    f.player.actor.save().progress().inventory.addPowerup(6, Damage::kInvulnerable, 0, 60);
    f.player.reaction = PlayerDeed::None;
    f.health.hurt(f.player, 1, HurtKind::Pierce, true, false, 1, f.events, impact, true, &stats);
    CHECK(f.player.reaction == PlayerDeed::None);
}

TEST_CASE("externally sounded melee retains damage and pain without an extra impact or cry",
          "[player-health][enemy-melee]") {
    Fixture f;
    f.hit(75, HurtKind::QuietBlow);
    CHECK(f.player.actor.save().health() == 925);
    CHECK(f.player.painOwed == 75);
    CHECK(f.player.hitFlashTicks == PlayerHealth::kHitFlashTicks);
    CHECK(f.player.hitSoundGap == 0);
    CHECK(f.sounds.empty());
    CHECK(f.cries.empty());
    f.hit(1);
    CHECK(f.player.painOwed == 46);
    CHECK(f.cries.size() == 1);
    CHECK(f.sounds.empty());
    f.player.actor.save().progress().health = 160;
    f.hit(15, HurtKind::QuietBlow);
    CHECK(f.named == std::vector<std::string>{"S_BADLY"});
    CHECK(f.player.painOwed == 61);
    f.hit(1000, HurtKind::QuietBlow);
    CHECK(f.player.life == PlayerLife::Dying);
    CHECK(f.sounds == std::vector<std::string>{"S_PLAYERDIES"});
    CHECK(f.cries.back() == "DIE2");
}

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

TEST_CASE("fractional damage survives between hits and rounds health rather than each blow",
          "[player-health][fractional-health]") {
    Fixture f;
    f.hit(0.25f);
    CHECK(f.player.actor.save().health() == 1000);
    f.hit(0.25f);
    CHECK(f.player.actor.save().health() == 1000); // 999.5, rounded for the HUD
    f.hit(0.25f);
    CHECK(f.player.actor.save().health() == 999);
    f.hit(0.25f);
    CHECK(f.player.actor.save().health() == 999);
    // An integer pickup must not erase a pending fraction either.
    f.hit(0.25f);
    f.player.actor.save().progress().health += 100;
    f.hit(0.5f);
    CHECK(f.player.actor.save().health() == 1098);
}

TEST_CASE("armor absorption does not consume fractional damage already owed",
          "[player-health][fractional-health]") {
    Fixture f;
    f.hit(0.25f);
    ClassStats stats;
    stats.armorMin = 400;
    stats.armorMax = 800;
    f.health.hurt(f.player, 2, HurtKind::Blow, false, false, 1, f.events, {}, false, &stats);
    f.hit(0.5f);
    CHECK(f.player.actor.save().health() == 999);
}

TEST_CASE("fractional health below one is lethal even when its HUD value rounds to one",
          "[player-health][fractional-health]") {
    Fixture f;
    f.player.actor.save().progress().health = 2;
    f.hit(1.0f);
    CHECK(f.player.life == PlayerLife::Standing);
    f.hit(0.25f);
    CHECK(f.player.life == PlayerLife::Dying);
    CHECK(f.player.healthFraction == 0);
}

TEST_CASE("gold invulnerability retains fractional healing between absorbed hits",
          "[player-health][fractional-health]") {
    Fixture f;
    f.player.actor.save().progress().inventory.addPowerup(powerup::kArmor,
                                                          Damage::kGoldInvulnerable, 0, 60);
    for (s32 i = 0; i < 4; ++i) {
        f.hit(2.5f);
    }
    CHECK(f.player.actor.save().health() == 1001);
}

TEST_CASE("a heavy blow taken unguarded teaches the guard to one who never blocked",
          "[game][screens][player-health]") {
    Fixture f;
    const auto heavy = [&](f32 damage, u32 flags) {
        f.health.hurt(f.player, damage, HurtKind::Blow, true, false, 1, f.events,
                      PlayerImpact{.flags = flags});
    };
    heavy(15, PlayerImpact::kKnockDown); // not over fifteen
    heavy(40, 0);                        // not heavy
    CHECK(f.blockLessons == 0);
    heavy(40, PlayerImpact::kKnockDown);
    CHECK(f.blockLessons == 1);
    f.player.blocked = true; // one who has blocked needs no lesson
    heavy(40, PlayerImpact::kKnockDown);
    CHECK(f.blockLessons == 1);
}

TEST_CASE("ordinary enemy damage flashes the skin without inventing a stagger",
          "[game][screens][player-health]") {
    Fixture f;
    f.hit(10, HurtKind::Blow, true);
    CHECK(f.player.hitFlashTicks == 0);
    f.hit(1);
    CHECK(f.player.hitFlashTicks == 0);
    f.hit(10);
    CHECK(f.player.hitFlashTicks == PlayerHealth::kHitFlashTicks);
    CHECK(f.player.reaction == PlayerDeed::None);
    f.player.hitFlashTicks = 1;
    f.hit(10);
    CHECK(f.player.hitFlashTicks == PlayerHealth::kHitFlashTicks);
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

TEST_CASE("player low health announcements take precedence over cries, not the blow's sound",
          "[game][screens][player-health]") {
    Fixture f;
    f.player.actor.save().progress().health = 151;
    f.hit(1, HurtKind::Burn);
    REQUIRE(f.named == std::vector<std::string>{"S_BADLY"});
    REQUIRE(f.cries.empty());
    REQUIRE(f.sounds.empty());
    // At fifty the line is a toss between the two (fn_8009FFF4 by frame parity).
    std::vector<std::string> last;
    PlayerRuntime other;
    other.actor.spawn(1, {}, nullptr, Vec3{0}, 0);
    for (s32 i = 0; i < 40; ++i) {
        other.actor.save().progress().health = 51;
        f.named.clear();
        f.health.hurt(other, 1, HurtKind::Gas, false, false, 1, f.events);
        REQUIRE(f.named.size() == 1);
        last.push_back(f.named.front());
    }
    CHECK(std::ranges::count(last, "S_LIFEFORCE") > 0);
    CHECK(std::ranges::count(last, "S_ABOUT") > 0);
    CHECK(std::ranges::count(last, "S_LIFEFORCE") + std::ranges::count(last, "S_ABOUT") == 40);
    REQUIRE(f.cries == std::vector<std::string>(40, "POISON"));
    f.cries.clear();
    // A blow that crosses a mark is still heard landing, and its harm still counts to a cry.
    f.player.actor.save().progress().health = 55;
    f.hit(10);
    CHECK(f.sounds == std::vector<std::string>{"S_PLYRDMG"});
    CHECK(f.player.painOwed == 10);
    CHECK(f.cries.empty());
}

TEST_CASE("the lowest standing player's heart beats faster and louder as health falls",
          "[game][screens][player-health]") {
    std::array<PlayerRuntime, 2> party;
    party[0].actor.spawn(0, {}, nullptr, Vec3{0}, 0);
    party[1].actor.spawn(1, {}, nullptr, Vec3{0}, 0);
    party[0].actor.save().progress().health = 300;
    party[1].actor.save().progress().health = 201;
    CHECK_FALSE(PlayerHealth::heartbeat(party, 2, false)); // nobody at two hundred
    party[1].actor.save().progress().health = 150;
    auto beat = PlayerHealth::heartbeat(party, 2, false);
    REQUIRE(beat);
    CHECK(beat->player == 1);
    CHECK(beat->volume == Approx(1.0f));
    // Two seconds between beats at a hundred and over; then one, then half of one.
    s32 ticks = 2;
    while (!PlayerHealth::heartbeat(party, 2, false)) {
        ticks += 2;
    }
    CHECK(ticks == 120);
    party[1].actor.save().progress().health = 20;
    party[1].heartbeatTicks = 0;
    beat = PlayerHealth::heartbeat(party, 2, false);
    REQUIRE(beat);
    CHECK(beat->volume == Approx(177.0f / 127.0f));
    CHECK(party[1].heartbeatTicks == 30);
    party[1].actor.save().progress().health = 5;
    party[1].heartbeatTicks = 0;
    CHECK(PlayerHealth::heartbeat(party, 2, false)->volume == Approx(202.0f / 127.0f));
    // The fallen are not heard; the lowest standing one is.
    party[1].life = PlayerLife::InTower;
    party[0].actor.save().progress().health = 60;
    beat = PlayerHealth::heartbeat(party, 2, false);
    REQUIRE(beat);
    CHECK(beat->player == 0);
    CHECK(beat->volume == Approx(152.0f / 127.0f));
    // Silent in the tower and while invulnerable, though the beat keeps its time.
    party[0].heartbeatTicks = 0;
    CHECK_FALSE(PlayerHealth::heartbeat(party, 2, true));
    CHECK(party[0].heartbeatTicks == 60);
    party[0].heartbeatTicks = 0;
    party[0].actor.save().progress().inventory.addPowerup(powerup::kArmor, powerup::kInvulnerable,
                                                          0, 60);
    CHECK_FALSE(PlayerHealth::heartbeat(party, 2, false));
}

TEST_CASE("player pain accumulates while burns and pierces retain their own cues",
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

TEST_CASE("gas cloud pulses choke between accumulated pain cries, not with the spike groan",
          "[player-health][gas-feedback]") {
    Fixture f;
    for (s32 pulse = 0; pulse < 6; ++pulse) {
        f.hit(10, HurtKind::Gas);
        CHECK(f.player.actor.save().health() == 1000 - 10 * (pulse + 1));
        CHECK(f.player.gagSeconds == Approx(1));
        CHECK(f.cries.back() != "DIE1");
    }
    CHECK(std::ranges::count_if(f.cries, [](const auto& cry) { return cry.starts_with("PAIN"); }) ==
          2);
    CHECK(std::ranges::count(f.cries, "POISON") == 4);
    CHECK(f.cries.size() == 6);
    CHECK(f.sounds.empty()); // No physical impact sound accompanies a gas cloud.
    CHECK(f.player.painOwed == 0);
}

TEST_CASE("a severe gas hit replaces the choking cue with one pain cry",
          "[player-health][gas-feedback]") {
    Fixture f;
    f.hit(61, HurtKind::Gas);
    REQUIRE(f.cries.size() == 1);
    CHECK(f.cries.front().starts_with("PAIN"));
    CHECK(f.player.painOwed == 0);
    CHECK(f.sounds.empty());
}

TEST_CASE("surviving hits request reactions after health gates and damage scaling",
          "[game][screens][player-health][player-impact]") {
    Fixture f;
    const PlayerImpact impact{PlayerImpact::kKnockDown, {0, 0, -1}};
    f.health.hurt(f.player, 10, HurtKind::Blow, true, true, 1, f.events, impact);
    REQUIRE(f.player.reaction == PlayerDeed::None);
    f.health.hurt(f.player, 10, HurtKind::Blow, true, false, 0.1f, f.events, impact);
    REQUIRE(f.player.reaction == PlayerDeed::None);
    f.health.hurt(f.player, 10, HurtKind::Blow, true, false, 1, f.events, impact);
    REQUIRE(f.player.reaction == PlayerDeed::FallBack);
    f.hit(1);
    REQUIRE(f.player.reaction == PlayerDeed::FallBack);
    f.player.reaction = PlayerDeed::None;
    f.player.actor.save().progress().health = 151;
    f.health.hurt(f.player, 10, HurtKind::Blow, true, false, 1, f.events, impact);
    REQUIRE(f.player.reaction == PlayerDeed::FallBack); // low-health cue cannot swallow it
    f.player.reaction = PlayerDeed::None;
    f.health.hurt(f.player, 1000, HurtKind::Blow, true, false, 1, f.events, impact);
    REQUIRE(f.player.life == PlayerLife::Dying);
    REQUIRE(f.player.reaction == PlayerDeed::None);
    f.health.hurt(f.player, 10, HurtKind::Blow, true, false, 1, f.events, impact);
    REQUIRE(f.player.reaction == PlayerDeed::None);
}
TEST_CASE("inventory protection reaches damage healing and reaction handling",
          "[game][items][player-health]") {
    Fixture f;
    auto& inventory = f.player.actor.save().progress().inventory;
    SECTION("silver invulnerability") {
        inventory.addPowerup(6, 0x10000, 0, 30);
        f.hit(100);
        CHECK(f.player.actor.save().health() == 1000);
        CHECK(f.player.hitFlashTicks == 0);
        inventory.powerups[0].on = false;
        f.hit(100);
        CHECK(f.player.actor.save().health() == 900);
    }
    SECTION("gold invulnerability converts scaled damage into health") {
        inventory.addPowerup(6, 0x110000, 0, 30);
        f.hit(100, HurtKind::Blow, false, 2);
        CHECK(f.player.actor.save().health() == 1020);
        CHECK(f.player.reaction == PlayerDeed::None);
        CHECK(f.sounds.empty());
    }
    SECTION("gas mask does not grant physical invulnerability") {
        inventory.addPowerup(6, 0x2008, 0, 15);
        f.hit(10, HurtKind::Gas);
        CHECK(f.player.actor.save().health() == 1000);
        f.hit(10);
        CHECK(f.player.actor.save().health() == 990);
    }
    SECTION("knockback armor strips the reaction without discarding damage") {
        inventory.addPowerup(6, 0x40000, 0, 15);
        f.health.hurt(f.player, 30, HurtKind::Blow, true, false, 1, f.events,
                      {PlayerImpact::kKnockDown, {0, 0, 1}});
        CHECK(f.player.actor.save().health() == 970);
        CHECK(f.player.reaction == PlayerDeed::None);
    }
}

TEST_CASE("an enemy arrow and bolt land with their own sounds, a blow with the plain one",
          "[player-health][enemy-missile]") {
    for (const auto& [flags, heard] :
         {std::pair{0x20000u, "S_PLYRDMG2"}, std::pair{0x40000u, "S_PLYRDMG3"},
          std::pair{0u, "S_PLYRDMG"}}) {
        Fixture f;
        f.health.hurt(f.player, 10, HurtKind::Blow, true, false, 1, f.events,
                      PlayerImpact{.flags = flags});
        CHECK(f.sounds == std::vector<std::string>{heard});
        CHECK(f.cries.empty());
    }
}

TEST_CASE("a hit that knocks pushes its victim the way it came, as guarding and harm allow",
          "[player-health][knockback]") {
    Fixture f;
    const PlayerImpact knock{.flags = PlayerImpact::kKnockDown, .direction = Vec3{0, 0, 1}};
    f.health.hurt(f.player, 10, HurtKind::Blow, true, false, 1, f.events, knock);
    REQUIRE(f.player.knockback.pending());
    f.player.knockback.kick(0.0f, false);
    CHECK(f.player.knockback.velocity().z == Approx(Knockback::kFallKick));
    // Two points or less knock nobody anywhere; a hit with no knock pushes nothing.
    Fixture light;
    light.health.hurt(light.player, 2, HurtKind::Blow, true, false, 1, light.events, knock);
    light.player.knockback.kick(0.0f, false);
    CHECK(light.player.knockback.velocity() == Vec3{0.0f});
    Fixture plain;
    plain.health.hurt(plain.player, 10, HurtKind::Blow, true, false, 1, plain.events,
                      PlayerImpact{.flags = 0, .direction = Vec3{0, 0, 1}});
    plain.player.knockback.kick(0.0f, false);
    CHECK(plain.player.knockback.velocity() == Vec3{0.0f});
    // In the tower nothing hurts, so nothing pushes.
    Fixture tower;
    tower.health.hurt(tower.player, 10, HurtKind::Blow, true, true, 1, tower.events, knock);
    CHECK_FALSE(tower.player.knockback.pending());
}
TEST_CASE("gas leaves its victim retching a second, Death's touch a frame",
          "[game][screens][player-health][pickup]") {
    Fixture f;
    f.health.hurt(f.player, 10, HurtKind::Gas, false, false, 1, f.events, {});
    CHECK(f.player.gagSeconds == Approx(1.0f));
    f.player.gagSeconds = 0.0f;
    f.health.hurt(f.player, 10, HurtKind::Blow, false, false, 1, f.events,
                  PlayerImpact{.flags = 0x1000});
    CHECK(f.player.gagSeconds == Approx(1.0f / 15.0f));
    f.player.gagSeconds = 0.0f;
    f.health.hurt(f.player, 10, HurtKind::Blow, false, false, 1, f.events, {});
    CHECK(f.player.gagSeconds == 0.0f);
}

} // namespace
