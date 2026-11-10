#include <array>
#include <cmath>
#include <string_view>

#include "expansions/shared/level/LegendsItems.h"
#include "formats/WorldFile.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/DamageTypes.h"
#include "game/combat/PowerupFlags.h"

namespace gdl::game {
namespace {

struct PowerupMapping {
    s32 kind;
    u32 nativeFlags;
    u32 flags;
};

// Dreamcast do_got_it (8c0ae6e0), including its mask literals at 8c0aedec.
// Keep this explicit even where the later GameCube item uses the same value:
// a new native property must not silently acquire an unrelated GDL effect.
constexpr std::array kPowerups{
    PowerupMapping{5, 1, 1},
    PowerupMapping{5, 2, 2},
    PowerupMapping{5, 3, 3},
    PowerupMapping{5, 4, 4},
    PowerupMapping{5, 0x80000, damage::kThreeWayShot},
    PowerupMapping{5, 0x400000, damage::kFiveWayShot},
    PowerupMapping{5, 0x100000, damage::kSuperShot},
    PowerupMapping{5, 0x200000, damage::kReflect},
    PowerupMapping{5, 0x10000000, damage::kHammer},
    PowerupMapping{5, 0x20000000, damage::kRapidFire},
    PowerupMapping{6, 0x10000, armor::kInvulnerable},
    PowerupMapping{6, 0x110000, armor::kInvulnerable | armor::kGoldInvulnerable},
    PowerupMapping{6, 0x20000, armor::kReflect},
    PowerupMapping{6, 0x80000, 0x80000}, // Halo / Anti-Death.
    PowerupMapping{6, 0x200000, armor::kFireArmor},
    PowerupMapping{6, 0x400000, armor::kElectricArmor},
    PowerupMapping{7, 0, 0},
    PowerupMapping{8, 0, 0},
    PowerupMapping{9, 1, powerup::kLevitation},
    PowerupMapping{9, 2, powerup::kXRay},
    PowerupMapping{9, 4, powerup::kInvisible},
    PowerupMapping{9, 8, powerup::kStopTime},
    PowerupMapping{9, 0x10, powerup::kFireBreath},
    PowerupMapping{9, 0x20, powerup::kAcidBreath},
    PowerupMapping{9, 0x40, powerup::kLightningBreath},
    PowerupMapping{9, 0x80, powerup::kPhoenix},
    PowerupMapping{9, 0x100, powerup::kGrowth},
    PowerupMapping{9, 0x200, powerup::kEnemyShrink},
    PowerupMapping{9, 0x400, powerup::kPojo},
    PowerupMapping{9, 0x1000, powerup::kSkorneHorns},
    PowerupMapping{9, 0x2000, powerup::kSkorneMask},
    PowerupMapping{9, 0x4000, powerup::kRightGauntlet},
    PowerupMapping{9, 0x8000, powerup::kLeftGauntlet},
};

bool finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

s32 parameter(const formats::ItemInstanceRecord& instance) {
    const u32 value =
        static_cast<u32>(instance.params[0]) | (static_cast<u32>(instance.params[1]) << 8U);
    return value < 0x8000U ? static_cast<s32>(value) : static_cast<s32>(value) - 0x10000;
}

} // namespace

std::optional<LegendsPickup> legendsDreamcastPickup(const formats::ItemInfoRecord& info,
                                                    const formats::ItemInstanceRecord& instance,
                                                    u32 nativeIndex, s32 realmType,
                                                    std::string* unsupported) {
    if (unsupported) {
        unsupported->clear();
    }
    const auto reject = [unsupported](std::string_view reason) -> std::optional<LegendsPickup> {
        if (unsupported) {
            *unsupported = reason;
        }
        return std::nullopt;
    };
    // Native containers (8c0ae4e0) reference definition rows, not placed pickup
    // indices. Their contents only become new items when the container opens.
    if (info.type != 1) {
        return std::nullopt;
    }
    if (instance.flags != 0 || info.objectFlags != 0 ||
        info.activeType != (info.subtype == 12 ? 0x16 : 0x10)) {
        return reject("Dreamcast pickup requires unsupported activation or visibility state");
    }
    if (!finite(instance.position) || !finite(instance.rotation) || !finite(info.collisionOffset) ||
        !std::isfinite(info.radius) || !std::isfinite(info.height) || info.radius <= 0 ||
        info.height < 0) {
        return reject("Invalid Dreamcast pickup collision bounds");
    }
    if (info.collisionFlags != 0 || info.collisionOffset != Vec3{0}) {
        return reject("Unsupported Dreamcast pickup collision flags or offset");
    }

    LegendsPickup result;
    result.nativeIndex = nativeIndex;
    result.position = instance.position;
    result.radius = info.radius;
    result.height = info.height;
    result.name = instance.name.empty() ? info.name : instance.name;
    if (info.subtype == 12) {
        if (info.collisionType != 3 || !std::isfinite(info.xSize) || !std::isfinite(info.zSize) ||
            info.xSize <= 0 || info.zSize <= 0 || std::abs(instance.rotation.x) > 0.000001f ||
            std::abs(instance.rotation.z) > 0.000001f) {
            return reject("Unsupported Dreamcast obelisk collision shape");
        }
        // SetItem (8c0a8340) expands the obelisk's horizontal bounds by 1.1.
        // Collision type 3 projects onto the item matrix's axes and compares
        // directly against xSize/zSize: these are half extents, not full widths.
        result.collision = LegendsPickupCollision::Box;
        result.radius *= 1.1f;
        result.xSize = info.xSize * 1.1f;
        result.zSize = info.zSize * 1.1f;
        result.yaw = instance.rotation.y; // Dreamcast's native item matrix preserves Y turns.
    } else if (info.collisionType != 1) {
        return reject("Unsupported Dreamcast pickup collision shape");
    }

    // SetItem copies properties/value/activeOn from +3c/+40/+4a. In particular,
    // a loose key does NOT use instance.params[0] as its count in this release.
    result.offer.amount = info.value;
    result.offer.strength = static_cast<f32>(info.activeOn);
    switch (info.subtype) {
    case 1:
    case 2:
        if (info.value <= 0 || info.properties != 0) {
            return reject("Unsupported Dreamcast gold or key properties");
        }
        result.offer.kind =
            info.subtype == 1 ? static_cast<s32>(ItemKind::Gold) : static_cast<s32>(ItemKind::Keys);
        break;
    case 3:
        if (info.value == 0 || info.properties != 0) {
            return reject("Unsupported Dreamcast food properties");
        }
        result.offer.kind = static_cast<s32>(ItemKind::Food);
        result.harmsPojo = info.name == "CHICKEN";
        break;
    case 4:
        if (info.value <= 0 || info.properties < 1 || info.properties > 4) {
            return reject("Unsupported Dreamcast potion properties");
        }
        // All 105 placed native potions have a fixed element, 1..4. The generic
        // definition with property 0 has no placements; its unresolved dynamic
        // colour cannot enter GDL's network profiles, which require 1..4.
        result.offer.kind = static_cast<s32>(ItemKind::Potion);
        result.offer.flags = std::array<u32, 4>{1, 2, 3, 4}[info.properties - 1];
        break;
    case 5:
    case 6:
    case 7:
    case 8:
    case 9: {
        if (info.value < 0 || info.activeOn < -1 || (info.value == 0 && info.activeOn == 0)) {
            return reject("Invalid Dreamcast powerup amount or duration");
        }
        bool known = false;
        for (const auto& mapping : kPowerups) {
            if (mapping.kind == info.subtype && mapping.nativeFlags == info.properties) {
                result.offer.kind = mapping.kind;
                result.offer.flags = mapping.flags;
                known = true;
                break;
            }
        }
        if (!known) {
            return reject("Unsupported Dreamcast powerup properties");
        }
        break;
    }
    case 10:
        if (info.value < 0 || info.value >= 13 || info.properties != 0) {
            return reject("Invalid Dreamcast runestone identity");
        }
        result.award = LegendsAward{LegendsRelic::Runestone, info.value};
        break;
    case 12: {
        const s32 index = parameter(instance);
        if (index < 0 || index >= 9 || info.properties != 0) {
            return reject("Invalid Dreamcast obelisk identity");
        }
        result.award = LegendsAward{LegendsRelic::Obelisk, index};
        // Unlike the other do_got_it cases, an obelisk does not enter the item
        // removal branch. It remains scenery after its one-time award.
        result.staysVisible = true;
        break;
    }
    case 13: {
        // Native table 8c14b614: the realm identity selects the quest item bit.
        // All authored quest item definitions have value=0; that is not its ID.
        constexpr std::array<s32, 9> kLegendByRealm{0, 2, 1, 4, 3, 5, 6, 0, 0};
        if (realmType < 0 || static_cast<usize>(realmType) >= kLegendByRealm.size() ||
            info.properties != 0) {
            return reject("Dreamcast legend item requires a valid native realm");
        }
        result.award =
            LegendsAward{LegendsRelic::LegendItem, kLegendByRealm[static_cast<usize>(realmType)]};
        break;
    }
    default:
        // BOSSKEY/subtype 11 is in the definition library but has no placed
        // instances and no case in the native pickup dispatcher. Boss rewards
        // need their own encounter flow; inventing a touch-to-shard award is wrong.
        return reject("Unsupported Dreamcast pickup subtype");
    }
    return result;
}

} // namespace gdl::game
