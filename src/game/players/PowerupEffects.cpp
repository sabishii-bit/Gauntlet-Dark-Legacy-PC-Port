#include "game/players/PowerupEffects.h"

#include <array>
#include <cmath>
#include <numbers>

#include "engine/core/Types.h"

namespace gdl::game {

namespace {

struct PowerupName {
    s32 kind;
    u32 flags;
    std::string_view text;
};

/** The original's list, in its order: the first entry whose flags a powerup carries names it;
 * an entry without flags is its kind's general name. */
constexpr std::array<PowerupName, 74> kNames{{
    {9, 0x00000001, "powerup.levitation"},
    {9, 0x00000002, "powerup.xray"},
    {9, 0x00000004, "powerup.invisible"},
    {9, 0x00000008, "powerup.stopTime"},
    {9, 0x00000010, "powerup.fireBreath"},
    {9, 0x00000020, "powerup.acidBreath"},
    {9, 0x00000040, "powerup.lightningBreath"},
    {9, 0x00000080, "powerup.phoenix"},
    {9, 0x00000100, "powerup.growth"},
    {9, 0x00000200, "powerup.enemyShrink"},
    {9, 0x00000400, "powerup.pojo"},
    {9, 0x00001000, "powerup.bossHorns"},
    {9, 0x00002000, "powerup.bossMask"},
    {9, 0x00004000, "powerup.bossGauntletRight"},
    {9, 0x00008000, "powerup.bossGauntletLeft"},
    {9, 0x00010000, "powerup.speed"},
    {9, 0x00020000, "powerup.health"},
    {9, 0x00040000, "powerup.dummy"},
    {9, 0x00080000, "powerup.turbo"},
    {9, 0x00100000, "powerup.mikey"},
    {9, 0x00200000, "powerup.handOfDeath"},
    {9, 0x00400000, "powerup.healthVamp"},
    {6, 0x00000001, "powerup.fireShield"},
    {6, 0x00000002, "powerup.elecShield"},
    {6, 0x00000004, "powerup.resistLight"},
    {6, 0x00000008, "powerup.gasMask"},
    {6, 0x00000010, "powerup.resistMagic"},
    {6, 0x00000100, "powerup.immuneFire"},
    {6, 0x00000200, "powerup.immuneElec"},
    {6, 0x00000400, "powerup.immuneLight"},
    {6, 0x00000800, "powerup.immuneAcid"},
    {6, 0x00001000, "powerup.immuneMagic"},
    {6, 0x00002000, "powerup.immuneGas"},
    {6, 0x00010000, "powerup.invulnerable"},
    {6, 0x00020000, "powerup.reflectiveArmor"},
    {6, 0x00040000, "powerup.knockbackArmor"},
    {6, 0x00080000, "powerup.antiDeath"},
    {6, 0x00100000, "powerup.goldInvulnerable"},
    {6, 0x00200000, "powerup.fireArmor"},
    {6, 0x00400000, "powerup.elecArmor"},
    {6, 0x00800000, "powerup.armorProtect"},
    {6, 0x01000000, "powerup.armorReflect"},
    {5, 0x00000010, "powerup.knockBack"},
    {5, 0x00000020, "powerup.knockDown"},
    {5, 0x00000040, "powerup.blownAway"},
    {5, 0x00000080, "powerup.stun"},
    {5, 0x00000100, "powerup.knockOver"},
    {5, 0x00000200, "powerup.magicWeapon"},
    {5, 0x00000400, "powerup.explode"},
    {5, 0x00000800, "powerup.poisonGas"},
    {5, 0x00001000, "powerup.deathStun"},
    {5, 0x00002000, "powerup.spike"},
    {5, 0x00004000, "powerup.grabbed"},
    {5, 0x00008000, "powerup.thrown"},
    {5, 0x00010000, "powerup.whirlwind"},
    {5, 0x00020000, "powerup.arrow"},
    {5, 0x00040000, "powerup.fireBall"},
    {5, 0x00080000, "powerup.threeWayShot"},
    {5, 0x00100000, "powerup.superShot"},
    {5, 0x00200000, "powerup.weaponReflect"},
    {5, 0x00400000, "powerup.fiveWayShot"},
    {5, 0x00800000, "powerup.weaponHeal"},
    {5, 0x01000000, "powerup.noHit"},
    {5, 0x02000000, "powerup.weaponTurbo"},
    {5, 0x04000000, "powerup.weaponSticky"},
    {5, 0x08000000, "powerup.weaponSticky"},
    {5, 0x10000000, "powerup.hammer"},
    {5, 0x20000000, "powerup.rapidFire"},
    {5, 0x40000000, "powerup.weaponLow"},
    {5, 0x00000000, "powerup.weapon"},
    {7, 0x00000000, "powerup.speedBoost"},
    {8, 0x00000000, "powerup.magicBoost"},
    {9, 0x00000000, "powerup.special"},
    {6, 0x00000000, "powerup.armor"},
}};

} // namespace

PowerupEffects PowerupEffects::of(const Inventory& inventory) {
    PowerupEffects effects;
    f32 elementTime = -1;
    for (const PowerupSlot& slot : inventory.powerups) {
        if (!slot.working()) {
            continue;
        }
        switch (slot.kind) {
        case powerup::kWeapon:
            // The low nibble is one element, not four independent flags.
            if ((slot.flags & 0xF) != 0 &&
                (elementTime < 0 || (slot.strength > 0 && slot.strength > elementTime))) {
                elementTime = slot.strength;
                effects.weapon = (effects.weapon & ~0xFU) | (slot.flags & 0xF);
            }
            effects.weapon |= slot.flags & ~0xFU;
            break;
        case powerup::kArmor: effects.armor |= slot.flags; break;
        case powerup::kSpeed:
            effects.paceAdd += slot.charge;
            effects.special |= powerup::kSpeedBoost;
            break;
        case powerup::kMagic: effects.magicAdd += slot.charge; break;
        case powerup::kSpecial: effects.special |= slot.flags; break;
        default: break;
        }
    }
    return effects;
}

f32 PowerupEffects::magicPower(s32 magicStat) const {
    constexpr f32 kStatScale = 0.001f;
    return kLeastMagicPower +
           kStatScale * static_cast<f32>(magicStat) * (kMostMagicPower - kLeastMagicPower) +
           magicAdd;
}

s32 PowerupEffects::shots() const {
    if ((weapon & powerup::kFiveWayShot) != 0) {
        return 5;
    }
    return (weapon & powerup::kThreeWayShot) != 0 ? 3 : 1;
}

f32 PowerupEffects::bodyAlpha(f32 seconds) const {
    if (!invisible()) {
        return 1.0f;
    }
    return kInvisibleAlpha - kInvisibleWaver * std::sin(2.0f * std::numbers::pi_v<f32> * seconds);
}

std::string_view powerupTextId(s32 kind, u32 flags) {
    for (const PowerupName& name : kNames) {
        if (name.kind == kind && (name.flags == 0 || (name.flags & flags) == name.flags)) {
            return name.text;
        }
    }
    return "powerup.unknown";
}

} // namespace gdl::game
