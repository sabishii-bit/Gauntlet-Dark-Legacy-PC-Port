#include "game/combat/Damage.h"

#include "game/combat/ArmorFlags.h"
#include "game/combat/DamageTypes.h"

namespace gdl::game {
Damage Damage::modify(f32 amount, u32 flags, u32 shield, f32 armorStat, bool bossEncounter) {
    if ((shield & armor::kGoldInvulnerable) != 0) {
        return {amount > 1 ? -0.1f * amount : 0, flags};
    }
    if ((shield & armor::kInvulnerable) != 0 ||
        ((shield & armor::kImmuneMagic) != 0 && (flags & damage::kMagic) != 0) ||
        ((shield & armor::kImmuneGas) != 0 && (flags & damage::kGas) != 0)) {
        return {0, flags};
    }
    const f32 weak = bossEncounter ? 0.75f : 0.5f;
    const f32 resist = bossEncounter ? 1.25f : 1.5f;
    const f32 strong = bossEncounter ? 1.5f : 2.0f;
    if ((shield & armor::kNoKnockback) != 0) {
        flags &= ~(damage::kKnocks | damage::kWhirlwind);
    }
    if ((shield & armor::kResistMagic) != 0 && (flags & damage::kMagic) != 0) {
        amount *= weak;
    }
    if ((flags & (damage::kMagic | damage::kGas)) == 0) {
        if (amount < 0) {
            amount = -amount;
        } else {
            amount = amount <= armorStat ? 0 : amount - armorStat;
        }
    }
    const u32 element = flags & damage::kElementMask;
    if (amount > 0 && element >= 1 && element < damage::kElementCount) {
        // The elements pair off: fire with lightning, light with acid.
        const u32 paired = ((element - 1) ^ 1U) + 1;
        if ((shield & armor::resist(element)) != 0) {
            amount *= weak;
        } else if ((shield & armor::immune(element)) != 0) {
            amount = 0;
        } else if ((shield & (armor::resist(paired) | armor::immune(paired))) != 0) {
            amount *= strong;
        } else {
            amount *= resist;
        }
    }
    return {amount, flags};
}
} // namespace gdl::game
