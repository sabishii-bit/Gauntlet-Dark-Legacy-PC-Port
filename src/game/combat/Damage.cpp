#include "game/combat/Damage.h"

namespace gdl::game {
Damage Damage::modify(f32 amount, u32 flags, u32 shield, f32 armor, bool bossEncounter) {
    if ((shield & kGoldInvulnerable) != 0) {
        return {amount > 1 ? -0.1f * amount : 0, flags};
    }
    if ((shield & kInvulnerable) != 0 || ((shield & 0x1000) != 0 && (flags & kMagic) != 0) ||
        ((shield & 0x2000) != 0 && (flags & kGas) != 0)) {
        return {0, flags};
    }
    const f32 weak = bossEncounter ? 0.75f : 0.5f;
    const f32 resist = bossEncounter ? 1.25f : 1.5f;
    const f32 strong = bossEncounter ? 1.5f : 2.0f;
    if ((shield & 0x40000) != 0) {
        flags &= ~0x10170U;
    }
    if ((shield & 0x10) != 0 && (flags & kMagic) != 0) {
        amount *= weak;
    }
    if ((flags & (kMagic | kGas)) == 0) {
        if (amount < 0) {
            amount = -amount;
        } else {
            amount = amount <= armor ? 0 : amount - armor;
        }
    }
    const u32 element = flags & kElement;
    if (amount > 0 && element >= 1 && element <= 4) {
        const u32 own = 1U << (element - 1);
        const u32 opposite = 1U << ((element - 1) ^ 1U);
        if ((shield & own) != 0) {
            amount *= weak;
        } else if ((shield & (own << 8)) != 0) {
            amount = 0;
        } else if ((shield & (opposite | (opposite << 8))) != 0) {
            amount *= strong;
        } else {
            amount *= resist;
        }
    }
    return {amount, flags};
}
} // namespace gdl::game
