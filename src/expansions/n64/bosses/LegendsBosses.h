#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "expansions/n64/formats/LegendsN64Scene.h"
#include "game/level/ImportedBoss.h"

namespace gdl::game {

/** A PPow kind/subtype, thrown by the native boss script rather than present in
 * the level's IPR table. It must still be collected using ordinary pickup rules. */
struct LegendsBossReward {
    u8 kind = 0;
    u8 subtype = 0;
    f32 angle = 0;
    f32 distance = 0;
};

struct LegendsBossEncounter {
    ImportedBoss combat;
    std::string map;
    u32 nativeRealm = 0;
    std::string nativeArchive; ///< provenance; combat intentionally uses the GDL counterpart
    std::vector<LegendsBossReward> rewards;
    f32 departureSeconds = 12;
    std::string destination = "tower2";
    s32 sharedObelisk = -1; ///< zero-based native identity, awarded after the death finishes
    s32 consumedLegend = -1;
    bool clearsRunestones = false;
    u32 finalNativeGrant = 0; ///< retained explicitly until its inventory consumer is supported
};

/** Reads US script tables, authenticating both executable overlays before using
 * fixed-address constants. Unknown/non-boss maps return no encounter. */
std::optional<LegendsBossEncounter>
legendsN64Boss(std::string_view map, std::span<const u8> program, std::span<const u8> bossOverlay);

/** Native TYPE camera fields, deliberately separate from the damage/reward origin. */
struct LegendsBossCamera {
    f32 floorOffset = 0;
    f32 attentionHeight = 0;
    f32 radius = 0;
};
LegendsBossCamera legendsN64BossCamera(std::span<const u8> moves);

/** Resolve scripted rewards through the native PPow table before any GPU resources
 * are loaded. IDs use a separate script namespace, never an IPR placement index. */
std::vector<formats::LegendsN64Fixture>
legendsN64BossRewardFixtures(const LegendsBossEncounter& encounter, std::span<const u8> powerups);

struct LegendsBossRewardMotion {
    Vec3 position{0};
    Vec3 velocity{0};
    f32 elapsed = 0;
};

/** e08898d8 supplies two positions to e0066864/e0076730, which computes a
 * speed20 ballistic throw with gravity64. Floor/flight collision belongs to the caller. */
std::optional<LegendsBossRewardMotion>
legendsBossRewardMotion(const LegendsBossReward& reward, Vec3 deathPosition, Vec3 playerStart);

/** Where e08898d8 throws a fallen boss's rewards about, wherever the boss fell: the place
 * e006b110 kept from its pose record as the level loaded (e0889850's e088f990), eight up
 * except in the Dragon's realm. */
Vec3 legendsN64BossRewardAnchor(const LegendsBossEncounter& encounter);

} // namespace gdl::game
