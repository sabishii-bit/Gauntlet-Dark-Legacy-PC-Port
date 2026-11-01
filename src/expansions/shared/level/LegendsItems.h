#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/math/Math.h"

#include "expansions/shared/characters/LegendsProgress.h"
#include "game/players/ItemPickup.h"

namespace gdl::formats {
struct LegendsN64Fixture;
struct LegendsN64Obelisk;
struct ItemInfoRecord;
struct ItemInstanceRecord;
} // namespace gdl::formats

namespace gdl::game {

struct LegendsAward {
    LegendsRelic kind = LegendsRelic::Runestone;
    s32 index = -1;
};

enum class LegendsPickupCollision : u8 { Cylinder, Box };

/** A placed native pickup translated into GDL mechanics. Native campaign awards
 * never use ItemKind::Runestone/Legend, which address the GameCube campaign. */
struct LegendsPickup {
    u32 nativeIndex = 0;
    Vec3 position{0};
    f32 radius = 0.5f;
    f32 height = 3.0f;
    LegendsPickupCollision collision = LegendsPickupCollision::Cylinder;
    f32 xSize = 0; ///< native box half extents
    f32 zSize = 0;
    f32 yaw = 0;
    ItemOffer offer;
    std::optional<LegendsAward> award;
    bool awardShared = false;      ///< native active-party award, not every stored character
    bool consumeWhenOwned = false; ///< native runes/shards disappear even on a repeat visit
    std::string name;
    bool collected = false;
    bool staysVisible = false;
    bool harmsPojo = false;               ///< native chicken food becomes -50 while Pojo is active
    bool active = true;                   ///< contained items wait for their opening animation
    std::vector<std::string> scrollPages; ///< verified native text, loaded before play
};

/** Unknown, hidden, or externally activated placements return no offer. Native
 * IDs/flags are translated explicitly, never copied into GDL powerup flags. */
std::optional<LegendsPickup> legendsN64Pickup(const formats::LegendsN64Fixture& fixture,
                                              std::string* unsupported = nullptr,
                                              s32 playerCount = 1);
std::optional<LegendsPickup> legendsN64ObeliskPickup(const formats::LegendsN64Obelisk& obelisk,
                                                     std::string* unsupported = nullptr,
                                                     s32 playerCount = 1);
std::optional<LegendsPickup> legendsDreamcastPickup(const formats::ItemInfoRecord& info,
                                                    const formats::ItemInstanceRecord& instance,
                                                    u32 nativeIndex, s32 realmType = -1,
                                                    std::string* unsupported = nullptr);

/** Uses the GDL pickup caps/inventory rules, leaving refused or partially taken
 * pickups available. Progression is written only to the selected Legends domain. */
ItemTaking takeLegendsPickup(LegendsPickup& pickup, LegendsPlatform platform, CharacterSave& save,
                             f32 powerupTime = 1, bool activatePowerups = true,
                             std::span<CharacterSave* const> sharedRecipients = {});

/** Native obelisks initialize spent when any joined character already owns the
 * bit (e00680b8). Invalid/null entries never alias another class or platform. */
bool legendsAwardOwnedByParty(const LegendsAward& award, LegendsPlatform platform,
                              std::span<CharacterSave* const> party);

bool touchesLegendsPickup(const LegendsPickup& pickup, const Vec3& position, f32 radius,
                          f32 height);

} // namespace gdl::game
