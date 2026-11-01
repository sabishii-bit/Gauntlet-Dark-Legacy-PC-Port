#include <array>
#include <cmath>
#include <string_view>

#include "expansions/n64/formats/LegendsN64Progress.h"
#include "expansions/n64/formats/LegendsN64Scene.h"
#include "expansions/shared/level/LegendsItems.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/DamageTypes.h"
#include "game/players/PowerupEffects.h"

namespace gdl::game {
namespace {

struct PowerupTranslation {
    u16 identity;
    s32 kind;
    u32 flags;
    bool charged;
};

// e00560a8 names the native inventory identities; e004c2f0 applies their effects.
// These are deliberately not the GameCube flag layout. The destination effects
// use GDL combat rules, while duration/use counts come from the native PPower.
constexpr std::array kPowerups{
    PowerupTranslation{0x840, powerup::kWeapon, damage::kReflect, false},
    PowerupTranslation{0x8b0, powerup::kWeapon, damage::kSuperShot, true},
    PowerupTranslation{0x020, powerup::kWeapon, damage::kThreeWayShot, false},
    PowerupTranslation{0x0e0, powerup::kSpecial, powerup::kInvisible, false},
    PowerupTranslation{0x0c0, powerup::kArmor, armor::kInvulnerable, false},
    PowerupTranslation{0x860, powerup::kWeapon, 1, false}, // fire amulet
    PowerupTranslation{0x8f0, powerup::kWeapon, 2, false}, // lightning amulet
    PowerupTranslation{0x870, powerup::kWeapon, 3, false}, // light amulet
    PowerupTranslation{0x880, powerup::kWeapon, 4, false}, // acid amulet
    PowerupTranslation{0x0d0, powerup::kSpecial, powerup::kStopTime, false},
    PowerupTranslation{0x0a0, powerup::kArmor, 0x00080000, false}, // anti-death
    PowerupTranslation{0x160, powerup::kWeapon, damage::kFiveWayShot, false},
    PowerupTranslation{0x100, powerup::kArmor, armor::kGoldInvulnerable | armor::kInvulnerable,
                       false},
    PowerupTranslation{0x460, powerup::kSpecial, powerup::kFireBreath, true},
    PowerupTranslation{0x4f0, powerup::kSpecial, powerup::kLightningBreath, true},
    PowerupTranslation{0x480, powerup::kSpecial, powerup::kAcidBreath, true},
    PowerupTranslation{0x110, powerup::kSpecial, powerup::kPhoenix, false},
    PowerupTranslation{0x120, powerup::kSpecial, powerup::kGrowth, false},
    PowerupTranslation{0x130, powerup::kSpecial, powerup::kEnemyShrink, false},
    PowerupTranslation{0xc40, powerup::kArmor, armor::kReflect, false},
    PowerupTranslation{0xc60, powerup::kArmor, armor::kFireArmor, false},
    PowerupTranslation{0xcf0, powerup::kArmor, armor::kElectricArmor, false},
    PowerupTranslation{0x940, powerup::kWeapon, damage::kHammer, true},
    PowerupTranslation{0x050, powerup::kWeapon, damage::kRapidFire, false},
    PowerupTranslation{0x030, powerup::kSpecial, powerup::kLevitation, false},
    PowerupTranslation{0x620, powerup::kSpecial, powerup::kSkorneMask, true},
    PowerupTranslation{0x630, powerup::kSpecial, powerup::kSkorneHorns, true},
    PowerupTranslation{0xa40, powerup::kSpecial, powerup::kRightGauntlet, true},
    PowerupTranslation{0xa50, powerup::kSpecial, powerup::kLeftGauntlet, true},
};

bool finite(Vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

} // namespace

std::optional<LegendsPickup> legendsN64Pickup(const formats::LegendsN64Fixture& fixture,
                                              std::string* unsupported, s32 playerCount) {
    if (unsupported != nullptr) {
        unsupported->clear();
    }
    const auto reject = [&](std::string_view reason) -> std::optional<LegendsPickup> {
        if (unsupported != nullptr) {
            *unsupported = reason;
        }
        return std::nullopt;
    };
    if (fixture.kind != formats::LegendsN64FixtureKind::Powerup) {
        return std::nullopt;
    }
    // e0064e60 compares the actual living party threshold before allocation.
    if (playerCount < 1 || playerCount > 4 || fixture.minimumPlayers > playerCount) {
        return std::nullopt;
    }
    if (fixture.sequence.empty()) {
        return reject("N64 pickup has no initial visible model");
    }
    if (!finite(fixture.position) || !std::isfinite(fixture.pickupRadius) ||
        !std::isfinite(fixture.pickupHeight) || fixture.pickupRadius <= 0 ||
        fixture.pickupHeight <= 0 || fixture.pickupRadius > 4 || fixture.pickupHeight > 16) {
        return reject("N64 pickup has invalid contact bounds");
    }
    const u32 flags = fixture.resolvedTemplateFlags;
    const u32 identity = flags & 0xffffU;
    const u8 kind = static_cast<u8>(fixture.resolvedTemplateKey >> 8U);
    const s32 subtype = static_cast<s32>(fixture.resolvedTemplateKey & 0xffU);
    // e0065618: only flag0x10000000 opts into the placement's signed quantity.
    const s32 amount = (flags & 0x10000000U) != 0 ? fixture.quantity : fixture.templateAmount;
    LegendsPickup pickup;
    pickup.nativeIndex = fixture.nativeIndex;
    pickup.position = fixture.position;
    pickup.radius = fixture.pickupRadius;
    pickup.height = fixture.pickupHeight;
    pickup.name = fixture.sequence;

    if (kind == 0x28 && fixture.quantity > 0) {
        pickup.offer = {static_cast<s32>(ItemKind::Scroll), fixture.quantity};
        return pickup;
    }

    // e0065618's rune/shard branch grants the subtype, not template amount.
    // These identities belong solely to the N64 campaign, never GC Relics.
    if (kind == 0x15 && identity == 0x8210 && subtype >= 1 && subtype <= 13) {
        pickup.award = LegendsAward{LegendsRelic::Runestone, subtype - 1};
        pickup.awardShared = true;
        pickup.consumeWhenOwned = true;
        return pickup;
    }
    if (kind == 0x2b && identity == 0x8260 && subtype >= 1 && subtype <= 4) {
        pickup.award = LegendsAward{LegendsRelic::Shard, subtype - 1};
        pickup.awardShared = true;
        pickup.consumeWhenOwned = true;
        return pickup;
    }
    if (kind == 0x29 && subtype >= 1 && subtype <= 5) {
        constexpr std::array<u32, 5> kIdentities{0x1c0, 0x1d0, 0x1e0, 0x1f0, 0x200};
        if (identity != kIdentities[static_cast<usize>(subtype - 1)]) {
            return reject("N64 legend pickup has an unknown native identity");
        }
        pickup.award = LegendsAward{LegendsRelic::LegendItem, subtype - 1};
        return pickup;
    }
    if (amount == 0 || (amount < 0 && kind != 4)) {
        return reject("N64 pickup has no positive grant");
    }
    if (kind == 0 && identity == 7) {
        pickup.offer = {static_cast<s32>(ItemKind::Keys), amount};
    } else if (kind == 3 && identity == 6) {
        pickup.offer = {static_cast<s32>(ItemKind::Gold), amount};
    } else if (kind == 4 && identity == 1) {
        pickup.offer = {static_cast<s32>(ItemKind::Food), amount};
    } else if (kind == 1 && subtype >= 1 && subtype <= 4 &&
               identity == (static_cast<u32>(subtype) << 12U | 8U)) {
        // Native potion IDs are blue/yellow/green/red (e004d790); GDL stores
        // fire/electric/light/acid. Preserve the actual element, not the number.
        constexpr std::array<u32, 4> kElements{2, 3, 4, 1};
        pickup.offer = {static_cast<s32>(ItemKind::Potion), amount,
                        kElements[static_cast<usize>(subtype - 1)]};
    } else if (kind == 2 || kind == 0x2a) {
        if (kind == 0x2a) {
            // e0065618 uses the ordinary charged item grant, not a permanent
            // armor-stat award. Preserve all twelve identity bits: masking to
            // 0x3f0 (the artwork lookup) loses its breath/weapon distinction.
            constexpr std::array<u32, 4> kIdentities{0x620, 0x630, 0xa40, 0xa50};
            if (subtype < 1 || subtype > 4 ||
                identity != kIdentities[static_cast<usize>(subtype - 1)]) {
                return reject("N64 Skorne reward has an unknown native identity");
            }
        }
        for (const auto& powerup : kPowerups) {
            if (powerup.identity != identity) {
                continue;
            }
            constexpr u32 kTimed = 0x400000;
            constexpr u32 kCharged = 0x800000;
            if ((flags & (kTimed | kCharged)) != (powerup.charged ? kCharged : kTimed)) {
                return reject("N64 powerup has an unsupported duration/use encoding");
            }
            // Native timed counters use150 ticks/second (e0054fdc/e00496d4).
            // A charged GDL slot has permanent lifetime until its uses run out.
            pickup.offer = {powerup.kind, powerup.charged ? amount : 0, powerup.flags,
                            powerup.charged ? -1.0f : static_cast<f32>(amount)};
            return pickup;
        }
        return reject("N64 powerup has no verified GDL mechanics mapping");
    } else {
        // Death, character coins, combined key/potion bundles,
        // and trigger/script awards need their own native gameplay consumers.
        return reject("N64 pickup requires an unsupported native gameplay consumer");
    }
    return pickup;
}

std::optional<LegendsPickup> legendsN64ObeliskPickup(const formats::LegendsN64Obelisk& obelisk,
                                                     std::string* unsupported, s32 playerCount) {
    if (unsupported != nullptr) {
        unsupported->clear();
    }
    if (playerCount < 1 || playerCount > 4 || obelisk.minimumPlayers > playerCount) {
        return std::nullopt;
    }
    if (!obelisk.supported || obelisk.identity < 1 || obelisk.identity > 7 ||
        !finite(obelisk.position) || !std::isfinite(obelisk.radius) ||
        !std::isfinite(obelisk.height) || obelisk.radius <= 0 || obelisk.height <= 0) {
        if (unsupported != nullptr) {
            *unsupported = "N64 obelisk has unsupported native identity or contact";
        }
        return std::nullopt;
    }
    LegendsPickup pickup;
    pickup.nativeIndex = obelisk.nativeIndex;
    pickup.position = obelisk.position;
    pickup.radius = obelisk.radius;
    pickup.height = obelisk.height;
    pickup.award = LegendsAward{LegendsRelic::Obelisk, static_cast<s32>(obelisk.identity - 1)};
    pickup.awardShared = true;
    pickup.consumeWhenOwned = true;
    pickup.staysVisible = true;
    pickup.name = obelisk.idleSequence;
    return pickup;
}

} // namespace gdl::game
