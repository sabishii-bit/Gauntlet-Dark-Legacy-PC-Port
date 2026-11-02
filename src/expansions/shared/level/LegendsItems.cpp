#include "expansions/shared/level/LegendsItems.h"

#include <algorithm>
#include <cmath>

#include "game/combat/PowerupFlags.h"
#include "game/players/PowerupEffects.h"

namespace gdl::game {

bool legendsAwardOwnedByParty(const LegendsAward& award, LegendsPlatform platform,
                              std::span<CharacterSave* const> party) {
    if (static_cast<usize>(platform) >= kLegendsPlatformCount) {
        return false;
    }
    return std::ranges::any_of(party, [&](const CharacterSave* save) {
        return save != nullptr && save->character >= 0 && save->character < kClassCount &&
               save->progress().legendsProgress(platform).has(award.kind, award.index);
    });
}

ItemTaking takeLegendsPickup(LegendsPickup& pickup, LegendsPlatform platform, CharacterSave& save,
                             f32 powerupTime, bool activatePowerups,
                             std::span<CharacterSave* const> sharedRecipients) {
    ItemTaking result;
    if (!pickup.active || pickup.collected ||
        static_cast<usize>(platform) >= kLegendsPlatformCount || save.character < 0 ||
        save.character >= kClassCount) {
        return result;
    }
    if (pickup.award) {
        // Validate the complete award before mutating any member of the party.
        LegendsProgress validation;
        if (!validation.add(pickup.award->kind, pickup.award->index)) {
            return result;
        }
        auto& progress = save.progress().legendsProgress(platform);
        const bool owned = progress.has(pickup.award->kind, pickup.award->index);
        bool granted = progress.add(pickup.award->kind, pickup.award->index);
        // e0065618 grants native runes/shards to all active slots; e0068000
        // does the same for obelisks. The caller supplies only the live roster,
        // not arbitrary save slots. add() is idempotent for duplicate pointers.
        if (pickup.awardShared) {
            for (auto* recipient : sharedRecipients) {
                if (recipient != nullptr && recipient->character >= 0 &&
                    recipient->character < kClassCount) {
                    granted |= recipient->progress().legendsProgress(platform).add(
                        pickup.award->kind, pickup.award->index);
                }
            }
        }
        if (owned && !granted && !pickup.consumeWhenOwned) {
            result.outcome = ItemTaking::Outcome::AlreadyHeld;
            return result;
        }
        result.outcome = ItemTaking::Outcome::Taken;
        result.count = 1;
        result.sound = "S_PICKUPRUNE";
    } else if (pickup.offer.kind == static_cast<s32>(ItemKind::Scroll)) {
        if (pickup.scrollPages.empty()) {
            return result;
        }
        result.outcome = ItemTaking::Outcome::Shown;
        result.count = pickup.offer.amount - 1;
    } else {
        // Only inventory goods cross this boundary. Passing native campaign IDs
        // to takeItem would accidentally modify the GameCube tower progression.
        if (pickup.offer.kind < static_cast<s32>(ItemKind::Gold) ||
            pickup.offer.kind > static_cast<s32>(ItemKind::SpecialPowerup) ||
            pickup.offer.amount < -1000000 || pickup.offer.amount > 1000000 ||
            ((pickup.offer.kind == static_cast<s32>(ItemKind::Gold) ||
              pickup.offer.kind == static_cast<s32>(ItemKind::Keys) ||
              pickup.offer.kind == static_cast<s32>(ItemKind::Potion)) &&
             pickup.offer.amount <= 0) ||
            !std::isfinite(pickup.offer.strength) || std::abs(pickup.offer.strength) > 1000000 ||
            !std::isfinite(powerupTime) || !std::isfinite(pickup.offer.strength * powerupTime) ||
            std::abs(pickup.offer.strength * powerupTime) > 1000000 || powerupTime <= 0) {
            return result;
        }
        auto offer = pickup.offer;
        if (pickup.harmsPojo && offer.kind == static_cast<s32>(ItemKind::Food) &&
            (PowerupEffects::of(save.progress().inventory).special & powerup::kPojo) != 0) {
            // Dreamcast do_got_it (8c0ae6e0) makes CHICKEN harmful to Pojo. Resolve
            // against the current wearer without changing the authored offer;
            // takeItem applies the normal poison/invulnerability rules.
            offer.amount = -50;
        }
        result = takeItem(save, offer, powerupTime, activatePowerups);
    }
    if (result.outcome == ItemTaking::Outcome::PartTaken) {
        pickup.offer.amount = result.left;
    } else if (result.took()) {
        pickup.collected = true;
    }
    return result;
}

bool touchesLegendsPickup(const LegendsPickup& pickup, const Vec3& position, f32 radius,
                          f32 height) {
    const Vec3 away = pickup.position - position;
    if (!pickup.active || pickup.collected || !std::isfinite(radius) || !std::isfinite(height) ||
        !std::isfinite(pickup.radius) || !std::isfinite(pickup.height) || radius < 0 ||
        height < 0 || pickup.radius < 0 || pickup.height < 0 || !std::isfinite(away.x) ||
        !std::isfinite(away.y) || !std::isfinite(away.z)) {
        return false;
    }
    // Same horizontal cylinder / vertical extent test as PlacedItems::Item.
    if (std::abs(away.y) > pickup.height + height) {
        return false;
    }
    const f32 reach = radius + pickup.radius;
    if (away.x * away.x + away.z * away.z > reach * reach) {
        return false;
    }
    if (pickup.collision == LegendsPickupCollision::Box) {
        if (!std::isfinite(pickup.xSize) || !std::isfinite(pickup.zSize) ||
            !std::isfinite(pickup.yaw) || pickup.xSize <= 0 || pickup.zSize <= 0) {
            return false;
        }
        const f32 c = std::cos(pickup.yaw);
        const f32 s = std::sin(pickup.yaw);
        return std::abs(c * away.x - s * away.z) <= pickup.xSize + radius &&
               std::abs(s * away.x + c * away.z) <= pickup.zSize + radius;
    }
    if (pickup.collision != LegendsPickupCollision::Cylinder) {
        return false;
    }
    return true;
}

} // namespace gdl::game
