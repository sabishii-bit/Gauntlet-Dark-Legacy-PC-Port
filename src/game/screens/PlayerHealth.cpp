#include "game/screens/PlayerHealth.h"

#include <cmath>
#include <format>

#include "engine/core/Log.h"
namespace gdl::game {
namespace {
constexpr float kPainEvery = 30.0f;    ///< harm from blows between cries
constexpr int kHeavyBlow = 60;         ///< a blow taking more than this is cried over at once
constexpr unsigned int kPainCries = 4; ///< S_<CLS>PAIN1 to 4
constexpr std::string_view kDeathSound = "S_PLAYERDIES";
constexpr std::string_view kHitSound = "S_PLYRDMG"; ///< a blow landing, now and then
constexpr int kHitSoundGapTicks = 30;
constexpr int kHealthLowMark = 150; ///< down to here: "needs food, badly"
constexpr int kHealthLastMark = 50; ///< and here: the life force, or about to die
constexpr std::string_view kBadlyLine = "S_BADLY";
constexpr std::string_view kLifeForceLine = "S_LIFEFORCE";
constexpr std::string_view kAboutToDieLine = "S_ABOUT";
} // namespace
float PlayerHealth::guarded(const PlayerRuntime& runtime, float damage, bool directed) {
    const PlayerFigure* figure = runtime.figure.get();
    if (figure == nullptr || damage <= 1.0f) {
        return damage;
    }
    if (figure->animator().defending()) {
        return directed ? damage * 0.5f : 0.0f;
    }
    return figure->animator().shoving() ? damage * 0.5f : damage;
}

void PlayerHealth::hurt(PlayerRuntime& runtime, float damage, HurtKind kind, bool directed,
                        bool inTower, float damageScale, const Events& events) {
    if (runtime.life != PlayerLife::Standing || inTower || damage <= 0.0f) {
        return;
    }
    const float unguarded = damage;
    damage = guarded(runtime, damage, directed);
    if (runtime.figure != nullptr && runtime.figure->animator().defending()) {
        events.block(unguarded - damage, damage);
    }
    if (damage <= 0.0f) {
        return;
    }
    if (damage > 1.0f) {
        damage *= damageScale;
    }
    CharacterSave& save = runtime.actor.save();
    const int left = save.health() - static_cast<int>(std::lround(damage));
    if (left < 1) {
        // Health of nought would read as a class never played: the fallen keep a point that
        // the status box does not show.
        save.progress().health = 1;
        runtime.life = PlayerLife::Dying;
        runtime.turbo.reset();
        events.sound(kDeathSound);
        events.cry("DIE2");
        log::info("Player {} has fallen", runtime.actor.player() + 1);
        return;
    }
    const int before = save.health();
    save.progress().health = left;
    // Crossing into low health is remarked on by name rather than cried over.
    if (before > kHealthLowMark && left <= kHealthLowMark) {
        events.named(kBadlyLine);
        return;
    }
    if (before > kHealthLastMark && left <= kHealthLastMark) {
        events.named((m_lowHealthTurn++ % 2 == 0) ? kLifeForceLine : kAboutToDieLine);
        return;
    }
    switch (kind) {
    case HurtKind::Burn:
        cryPain(events);
        runtime.painOwed = 0.0f;
        break;
    case HurtKind::Pierce: events.cry("DIE1"); break;
    case HurtKind::Gas: events.cry("POISON"); break;
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
        } else if (runtime.hitSoundGap <= 0) {
            events.sound(kHitSound);
            runtime.hitSoundGap = kHitSoundGapTicks;
        }
        break;
    }
}

void PlayerHealth::cryPain(const Events& events) {
    const int which = 1 + static_cast<int>(m_painRandom() % kPainCries);
    events.cry(std::format("PAIN{}", which));
}
} // namespace gdl::game
