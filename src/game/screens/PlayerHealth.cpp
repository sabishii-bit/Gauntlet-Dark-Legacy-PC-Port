#include "game/screens/PlayerHealth.h"

#include <cmath>
#include <format>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

#include "game/combat/Damage.h"
#include "game/players/PowerupEffects.h"
#include "game/players/Progression.h"
namespace gdl::game {
namespace {
constexpr f32 kPainEvery = 30.0f; ///< harm from blows between cries
constexpr s32 kHeavyBlow = 60;    ///< a blow taking more than this is cried over at once
constexpr u32 kPainCries = 4;     ///< S_<CLS>PAIN1 to 4
constexpr std::string_view kDeathSound = "S_PLAYERDIES";
constexpr std::string_view kHitSound = "S_PLYRDMG"; ///< a blow landing, now and then
constexpr std::string_view kArrowHitSound = "S_PLYRDMG2";
constexpr std::string_view kBoltHitSound = "S_PLYRDMG3";
constexpr s32 kHitSoundGapTicks = 30;
constexpr s32 kHealthLowMark = 150; ///< down to here: "needs food, badly"
constexpr s32 kHealthLastMark = 50; ///< and here: the life force, or about to die
constexpr std::string_view kBadlyLine = "S_BADLY";
constexpr std::string_view kLifeForceLine = "S_LIFEFORCE";
constexpr std::string_view kAboutToDieLine = "S_ABOUT";
constexpr f32 kLowHealthWait = 1.0f; ///< seconds a line waits behind narration (fn_8009FFF4)
constexpr f32 kAboutToDieWait = 0.5f;
constexpr u32 kDrainFlag = 0x1000;             ///< Death's touch
constexpr f32 kGasGagSeconds = 1.0f;           ///< retching after gas
constexpr f32 kDrainGagSeconds = 1.0f / 15.0f; ///< and after Death's touch

f32 liveHealth(const PlayerRuntime& runtime) {
    return static_cast<f32>(runtime.actor.save().health()) + runtime.healthFraction;
}

void storeHealth(PlayerRuntime& runtime, f32 health) {
    const auto rounded = static_cast<s32>(std::lround(health));
    runtime.actor.save().progress().health = rounded;
    runtime.healthFraction = health - static_cast<f32>(rounded);
}
} // namespace
f32 PlayerHealth::guarded(const PlayerRuntime& runtime, f32 damage, bool directed) {
    const PlayerFigure* figure = runtime.figure.get();
    if (figure == nullptr || damage <= 1.0f) {
        return damage;
    }
    if (figure->animator().defending()) {
        return directed ? damage * 0.5f : 0.0f;
    }
    return figure->animator().shoving() ? damage * 0.5f : damage;
}

bool PlayerHealth::canBeDamaged(const PlayerRuntime& runtime) {
    return runtime.life == PlayerLife::Standing && !runtime.combo.active() &&
           (runtime.figure == nullptr || !runtime.figure->animator().damageProtected());
}

bool PlayerHealth::canTakeSurfaceDamage(const PlayerRuntime& runtime) {
    return canBeDamaged(runtime) && !runtime.capture.active() &&
           runtime.reaction == PlayerDeed::None &&
           (runtime.figure == nullptr || !runtime.figure->animator().reacting());
}

void PlayerHealth::hurt(PlayerRuntime& runtime, f32 damage, HurtKind kind, bool directed,
                        bool inTower, f32 damageScale, const Events& events,
                        const PlayerImpact& impact, bool bossEncounter, const ClassStats* stats) {
    if (!canBeDamaged(runtime) || inTower || damage < 0.0f ||
        (impact.flags & Damage::kMagic) != 0) {
        return;
    }
    if (damage > 1.0f && kind != HurtKind::DeathDrain) {
        damage *= damageScale;
    }
    const auto worn = PowerupEffects::of(runtime.actor.save().progress().inventory);
    PlayerImpact received = impact;
    if (kind == HurtKind::Gas) {
        received.flags |= Damage::kGas;
    }
    const f32 armor = stats != nullptr ? armorDefense(*stats, runtime.actor.save().progress()) : 0;
    const Damage modified = Damage::modify(kind == HurtKind::DeathDrain ? -damage : damage,
                                           received.flags, worn.armor, armor, bossEncounter);
    damage = modified.amount;
    received.flags = modified.flags;
    if ((received.flags & Damage::kLow) != 0 && (worn.special & powerup::kLevitation) != 0) {
        return;
    }
    if (damage < 0) {
        storeHealth(runtime, liveHealth(runtime) - damage);
        return;
    }
    const f32 unguarded = damage;
    if (kind != HurtKind::DeathDrain) {
        damage = guarded(runtime, damage, directed);
    }
    if (runtime.figure != nullptr && runtime.figure->animator().defending()) {
        events.block(unguarded - damage, damage);
    } else if (unguarded > kBlockLessonFrom && (received.flags & kHeavyFlags) != 0 &&
               !runtime.blocked && events.learnBlock) {
        // A heavy blow taken in the face teaches the guard (damage_player, player.c 3400).
        events.learnBlock();
    }
    if (damage <= 0.0f) {
        // Stun shots and sticky hits retain their reaction with no health loss.
        // PlayerKnockback tests it before hit_damage, but invulnerability's
        // 0x10000 shield clears all reactions first (pmotion.c).
        if ((worn.armor & Damage::kInvulnerable) == 0) {
            runtime.reaction = PlayerImpact::combine(
                runtime.reaction, received.reaction(0, runtime.actor.yaw(), false));
        }
        return;
    }
    if (damage > 1.0f) {
        runtime.hitFlashTicks = kHitFlashTicks;
    }
    if (events.vibrate) {
        const bool braced = runtime.figure != nullptr && (runtime.figure->animator().defending() ||
                                                          runtime.figure->animator().shoving());
        const u32 flags = received.effective(damage, braced);
        // damage_player 80078B18..80078B7C: feedback follows post-armor/guard flags,
        // including lethal blows; harmless contacts and healing never reach this branch.
        s32 frames = 10;
        if ((flags & 0x10040) != 0) {
            frames = 30;
        } else if ((flags & 0x120) != 0) {
            frames = 20;
        } else if ((flags & 0x90) != 0) {
            frames = 15;
        }
        events.vibrate(frames);
    }
    // Gas leaves its victim retching a second, Death's touch a frame (damage_player,
    // player.c 3442).
    if ((received.flags & Damage::kGas) != 0) {
        runtime.gagSeconds = kGasGagSeconds;
    }
    if ((received.flags & kDrainFlag) != 0) {
        runtime.gagSeconds = kDrainGagSeconds;
    }
    CharacterSave& save = runtime.actor.save();
    const f32 remaining = liveHealth(runtime) - damage;
    if (remaining < 1.0f) {
        // Health of nought would read as a class never played: the fallen keep a point that
        // the status box does not show.
        save.progress().health = 1;
        runtime.healthFraction = 0;
        runtime.life = PlayerLife::Dying;
        runtime.turbo.reset();
        events.sound(kDeathSound);
        events.cry("DIE2");
        log::info("Player {} has fallen", runtime.actor.player() + 1);
        return;
    }
    const s32 before = save.health();
    storeHealth(runtime, remaining);
    const s32 left = save.health();
    if (kind == HurtKind::QuietBlow) {
        runtime.painOwed += damage;
    }
    const bool braced = runtime.figure != nullptr && (runtime.figure->animator().defending() ||
                                                      runtime.figure->animator().shoving());
    runtime.reaction = PlayerImpact::combine(
        runtime.reaction, received.reaction(damage, runtime.actor.yaw(), braced));
    // What knocks the body pushes it, along the way the hit came (damage_player's hit_force).
    runtime.knockback.queue(received.direction, received.effective(damage, braced), damage);
    // Crossing into low health is remarked on by name rather than cried over, though a blow
    // still lands with its sound. Which of the last two lines is heard is a toss.
    const bool low = before > kHealthLowMark && left <= kHealthLowMark;
    const bool last = !low && before > kHealthLastMark && left <= kHealthLastMark;
    if (low || last) {
        const bool lifeForce = !low && m_painRandom() % 2 == 0;
        if (low || lifeForce) {
            events.named(low ? kBadlyLine : kLifeForceLine, kLowHealthWait);
        } else {
            events.named(kAboutToDieLine, kAboutToDieWait);
        }
        if (kind == HurtKind::Blow || kind == HurtKind::Gas) {
            runtime.painOwed += damage;
            landBlow(runtime, events, received.flags);
        }
        return;
    }
    switch (kind) {
    case HurtKind::DeathDrain:
    case HurtKind::QuietBlow: break;
    case HurtKind::Burn:
        cryPain(events);
        runtime.painOwed = 0.0f;
        break;
    case HurtKind::Pierce: events.cry("DIE1"); break;
    case HurtKind::Gas:
    case HurtKind::Blow:
        // A heavy blow gets a cry at once; lesser ones add up to one, and land with the
        // sound of the hit itself now and then.
        runtime.painOwed += damage;
        if (before - left > kHeavyBlow) {
            runtime.painOwed = 0.0f;
            cryPain(events);
        } else if (runtime.painOwed >= kPainEvery) {
            runtime.painOwed -= kPainEvery;
            cryPain(events);
        } else {
            landBlow(runtime, events, received.flags);
        }
        break;
    }
}

/** A blow landing is heard now and then: an arrow's and a bolt's each their own
 * (AudioPlayerHit's rows). */
void PlayerHealth::landBlow(PlayerRuntime& runtime, const Events& events, u32 flags) {
    // Cloud damage uses the queued choking bark; piercing traps use the direct groan.
    if ((flags & Damage::kGas) != 0) {
        events.cry("POISON");
        return;
    }
    constexpr u32 kArrowHit = 0x20000;
    constexpr u32 kBoltHit = 0x40000;
    if (runtime.hitSoundGap <= 0) {
        if ((flags & kArrowHit) != 0) {
            events.sound(kArrowHitSound);
        } else if ((flags & kBoltHit) != 0) {
            events.sound(kBoltHitSound);
        } else {
            events.sound(kHitSound);
        }
        runtime.hitSoundGap = kHitSoundGapTicks;
    }
}

std::optional<PlayerHealth::Heartbeat> PlayerHealth::heartbeat(std::span<PlayerRuntime> players,
                                                               s32 ticks, bool inTower) {
    constexpr s32 kSteadyHealth = 100;
    constexpr s32 kFaintHealth = 25;
    constexpr s32 kFailingHealth = 10;
    constexpr s32 kSteadyTicks = 120;
    constexpr s32 kFaintTicks = 60;
    constexpr s32 kFailingTicks = 30;
    constexpr f32 kBaseLevel = 127.0f; ///< the original's level for the sound, played as 1
    constexpr f32 kFaintLevel = 152.0f;
    constexpr f32 kFailingLevel = 177.0f;
    constexpr f32 kLastLevel = 202.0f;
    PlayerRuntime* lowest = nullptr;
    usize index = 0;
    for (usize i = 0; i < players.size(); ++i) {
        PlayerRuntime& runtime = players[i];
        if (runtime.life == PlayerLife::Standing &&
            (lowest == nullptr || runtime.actor.save().health() < lowest->actor.save().health())) {
            lowest = &runtime;
            index = i;
        }
    }
    if (lowest == nullptr) {
        return std::nullopt;
    }
    const s32 health = lowest->actor.save().health();
    if (health > kHeartbeatHealth) {
        return std::nullopt;
    }
    lowest->heartbeatTicks -= ticks;
    if (lowest->heartbeatTicks > 0) {
        return std::nullopt;
    }
    if (health >= kSteadyHealth) {
        lowest->heartbeatTicks = kSteadyTicks;
    } else if (health >= kFaintHealth) {
        lowest->heartbeatTicks = kFaintTicks;
    } else {
        lowest->heartbeatTicks = kFailingTicks;
    }
    const u32 armor = PowerupEffects::of(lowest->actor.save().progress().inventory).armor;
    if (inTower || (armor & (powerup::kInvulnerable | powerup::kGoldInvulnerable)) != 0) {
        return std::nullopt;
    }
    f32 level = kBaseLevel;
    if (health <= kFailingHealth) {
        level = kLastLevel;
    } else if (health < kFaintHealth) {
        level = kFailingLevel;
    } else if (health < kSteadyHealth) {
        level = kFaintLevel;
    }
    return Heartbeat{index, level / kBaseLevel};
}

void PlayerHealth::cryPain(const Events& events) {
    const s32 which = 1 + static_cast<s32>(m_painRandom() % kPainCries);
    events.cry(std::format("PAIN{}", which));
}
} // namespace gdl::game
