#include "game/players/PowerupEffects.h"

#include <algorithm>
#include <array>
#include <numbers>

#include "engine/core/Types.h"
#include "engine/math/PortableMath.h"

#include "game/combat/ArmorFlags.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/PowerupFlags.h"

namespace gdl::game {

bool powerupAllowedInChallenge(s32 kind, u32 flags) {
    return kind != powerup::kSpecial || (flags & powerup::kStopTime) == 0;
}

namespace {

struct PowerupName {
    s32 kind;
    u32 flags;
    std::string_view text;
};

/** The first entry whose flags a powerup carries names it; an entry without flags is its
 * kind's general name. Gold precedes Silver to distinguish their shared invulnerability bit. */
constexpr std::array<PowerupName, 74> kNames{{
    {powerup::kSpecial, powerup::kLevitation, "powerup.levitation"},
    {powerup::kSpecial, powerup::kXRay, "powerup.xray"},
    {powerup::kSpecial, powerup::kInvisible, "powerup.invisible"},
    {powerup::kSpecial, powerup::kStopTime, "powerup.stopTime"},
    {powerup::kSpecial, powerup::kFireBreath, "powerup.fireBreath"},
    {powerup::kSpecial, powerup::kAcidBreath, "powerup.acidBreath"},
    {powerup::kSpecial, powerup::kLightningBreath, "powerup.lightningBreath"},
    {powerup::kSpecial, powerup::kPhoenix, "powerup.phoenix"},
    {powerup::kSpecial, powerup::kGrowth, "powerup.growth"},
    {powerup::kSpecial, powerup::kEnemyShrink, "powerup.enemyShrink"},
    {powerup::kSpecial, powerup::kPojo, "powerup.pojo"},
    {powerup::kSpecial, powerup::kSkorneHorns, "powerup.bossHorns"},
    {powerup::kSpecial, powerup::kSkorneMask, "powerup.bossMask"},
    {powerup::kSpecial, powerup::kRightGauntlet, "powerup.bossGauntletRight"},
    {powerup::kSpecial, powerup::kLeftGauntlet, "powerup.bossGauntletLeft"},
    {powerup::kSpecial, powerup::kSpeedBoost, "powerup.speed"},
    {powerup::kSpecial, powerup::kHealth, "powerup.health"},
    {powerup::kSpecial, powerup::kDummy, "powerup.dummy"},
    {powerup::kSpecial, powerup::kTurbo, "powerup.turbo"},
    {powerup::kSpecial, powerup::kMikey, "powerup.mikey"},
    {powerup::kSpecial, powerup::kHandOfDeath, "powerup.handOfDeath"},
    {powerup::kSpecial, powerup::kHealthVamp, "powerup.healthVamp"},
    {powerup::kArmor, armor::kResistFire, "powerup.fireShield"},
    {powerup::kArmor, armor::kResistElectric, "powerup.elecShield"},
    {powerup::kArmor, armor::kResistLight, "powerup.resistLight"},
    {powerup::kArmor, armor::kResistAcid, "powerup.gasMask"},
    {powerup::kArmor, armor::kResistMagic, "powerup.resistMagic"},
    {powerup::kArmor, armor::kImmuneFire, "powerup.immuneFire"},
    {powerup::kArmor, armor::kImmuneElectric, "powerup.immuneElec"},
    {powerup::kArmor, armor::kImmuneLight, "powerup.immuneLight"},
    {powerup::kArmor, armor::kImmuneAcid, "powerup.immuneAcid"},
    {powerup::kArmor, armor::kImmuneMagic, "powerup.immuneMagic"},
    {powerup::kArmor, armor::kImmuneGas, "powerup.immuneGas"},
    {powerup::kArmor, armor::kGoldInvulnerable, "powerup.goldInvulnerable"},
    {powerup::kArmor, armor::kInvulnerable, "powerup.invulnerable"},
    {powerup::kArmor, armor::kReflect, "powerup.reflectiveArmor"},
    {powerup::kArmor, armor::kNoKnockback, "powerup.knockbackArmor"},
    {powerup::kArmor, armor::kAntiDeath, "powerup.antiDeath"},
    {powerup::kArmor, armor::kFireArmor, "powerup.fireArmor"},
    {powerup::kArmor, armor::kElectricArmor, "powerup.elecArmor"},
    {powerup::kArmor, armor::kProtect, "powerup.armorProtect"},
    {powerup::kArmor, armor::kReflect2, "powerup.armorReflect"},
    {powerup::kWeapon, damage::kKnockBack, "powerup.knockBack"},
    {powerup::kWeapon, damage::kKnockDown, "powerup.knockDown"},
    {powerup::kWeapon, damage::kBlownAway, "powerup.blownAway"},
    {powerup::kWeapon, damage::kStun, "powerup.stun"},
    {powerup::kWeapon, damage::kKnockOver, "powerup.knockOver"},
    {powerup::kWeapon, damage::kMagic, "powerup.magicWeapon"},
    {powerup::kWeapon, damage::kExplode, "powerup.explode"},
    {powerup::kWeapon, damage::kGas, "powerup.poisonGas"},
    {powerup::kWeapon, damage::kDeathStun, "powerup.deathStun"},
    {powerup::kWeapon, damage::kSpike, "powerup.spike"},
    {powerup::kWeapon, damage::kGrabbed, "powerup.grabbed"},
    {powerup::kWeapon, damage::kThrown, "powerup.thrown"},
    {powerup::kWeapon, damage::kWhirlwind, "powerup.whirlwind"},
    {powerup::kWeapon, damage::kArrow, "powerup.arrow"},
    {powerup::kWeapon, damage::kFireBall, "powerup.fireBall"},
    {powerup::kWeapon, damage::kThreeWayShot, "powerup.threeWayShot"},
    {powerup::kWeapon, damage::kSuperShot, "powerup.superShot"},
    {powerup::kWeapon, damage::kReflect, "powerup.weaponReflect"},
    {powerup::kWeapon, damage::kFiveWayShot, "powerup.fiveWayShot"},
    {powerup::kWeapon, damage::kHeal, "powerup.weaponHeal"},
    {powerup::kWeapon, damage::kNoHitEffect, "powerup.noHit"},
    {powerup::kWeapon, damage::kTurbo, "powerup.weaponTurbo"},
    {powerup::kWeapon, damage::kSticky, "powerup.weaponSticky"},
    {powerup::kWeapon, damage::kBossHit, "powerup.weaponSticky"},
    {powerup::kWeapon, damage::kHammer, "powerup.hammer"},
    {powerup::kWeapon, damage::kRapidFire, "powerup.rapidFire"},
    {powerup::kWeapon, damage::kLow, "powerup.weaponLow"},
    {powerup::kWeapon, 0, "powerup.weapon"},
    {powerup::kSpeed, 0, "powerup.speedBoost"},
    {powerup::kMagic, 0, "powerup.magicBoost"},
    {powerup::kSpecial, 0, "powerup.special"},
    {powerup::kArmor, 0, "powerup.armor"},
}};

} // namespace

namespace {

/** The original's choice of the time a powerup shows (PlayerProcessPowerups): a slot for good
 * (under none) or with longer left replaces what was found so far. */
f32 longerOf(f32 found, f32 left) {
    return left < 0.0f || left > found ? left : found;
}

} // namespace

PowerupEffects PowerupEffects::of(const Inventory& inventory) {
    PowerupEffects effects;
    effects.special = inventory.permanentPojo ? powerup::kPojo : 0;
    f32 elementTime = -1;
    for (const PowerupSlot& slot : inventory.powerups) {
        if (!slot.working()) {
            continue;
        }
        switch (slot.kind) {
        case powerup::kWeapon:
            // The low nibble is one element, not four independent flags.
            if ((slot.flags & damage::kElementMask) != 0 &&
                (elementTime < 0 || (slot.strength > 0 && slot.strength > elementTime))) {
                elementTime = slot.strength;
                effects.weapon =
                    (effects.weapon & ~damage::kElementMask) | (slot.flags & damage::kElementMask);
            }
            effects.weapon |= slot.flags & ~damage::kElementMask;
            break;
        case powerup::kArmor:
            effects.armor |= slot.flags;
            if ((slot.flags & armor::kInvulnerable) != 0) {
                effects.invulnerableLeft = longerOf(effects.invulnerableLeft, slot.strength);
            }
            break;
        case powerup::kSpeed:
            effects.paceAdd += slot.charge;
            effects.special |= powerup::kSpeedBoost;
            break;
        case powerup::kMagic: effects.magicAdd += slot.charge; break;
        case powerup::kSpecial:
            effects.special |= slot.flags;
            if ((slot.flags & powerup::kInvisible) != 0) {
                effects.invisibleLeft = longerOf(effects.invisibleLeft, slot.strength);
            }
            break;
        default: break;
        }
    }
    return effects;
}

f32 PowerupEffects::magicPower(s32 magicStat) const {
    constexpr f32 kStatScale = 0.001f;
    // What the powerups add still stays within the range (SetPlayerVars' clamps).
    return std::clamp(kLeastMagicPower +
                          kStatScale * static_cast<f32>(magicStat) *
                              (kMostMagicPower - kLeastMagicPower) +
                          magicAdd,
                      kLeastMagicPower, kMostMagicPower);
}

s32 PowerupEffects::shots() const {
    if ((weapon & damage::kFiveWayShot) != 0) {
        return 5;
    }
    return (weapon & damage::kThreeWayShot) != 0 ? 3 : 1;
}

bool PowerupEffects::blinkedOff(f32 left) {
    return left >= 0.0f && left <= kWarningSeconds &&
           (static_cast<s32>(kBlinkRate * left) & 1) == 0;
}

f32 PowerupEffects::bodyAlpha() const {
    if (!invisible() || blinkedOff(invisibleLeft)) {
        return 1.0f;
    }
    return kInvisibleAlpha -
           kInvisibleWaver * portable::sin(2.0f * std::numbers::pi_v<f32> * invisibleLeft);
}

PowerupEffects::Chrome PowerupEffects::chrome() const {
    if ((armor & (armor::kGoldInvulnerable | armor::kInvulnerable)) == 0 ||
        blinkedOff(invulnerableLeft)) {
        return Chrome::None;
    }
    return (armor & armor::kGoldInvulnerable) != 0 ? Chrome::Gold : Chrome::Silver;
}

std::string_view powerupTextId(s32 kind, u32 flags) {
    const auto named = [&](bool flagged) -> std::string_view {
        for (const PowerupName& name : kNames) {
            if (name.kind == kind && (name.flags != 0) == flagged &&
                (name.flags & flags) == name.flags) {
                return name.text;
            }
        }
        return {};
    };
    if (const auto text = named(true); !text.empty()) {
        return text;
    }
    // An amulet keeps its element in the weapon's low nibble (items.c's pickup dispatch), not
    // in flag bits, so it is named after the flagged weapons and before the general name.
    static constexpr std::array<std::string_view, 4> kAmulets{
        "powerup.fireAmulet", "powerup.lightningAmulet", "powerup.lightAmulet",
        "powerup.acidAmulet"};
    const u32 element = flags & damage::kElementMask;
    if (kind == powerup::kWeapon && element >= 1 && element <= kAmulets.size()) {
        return kAmulets[element - 1];
    }
    if (const auto text = named(false); !text.empty()) {
        return text;
    }
    return "powerup.unknown";
}

} // namespace gdl::game
