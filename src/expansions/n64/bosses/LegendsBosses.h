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

/** Native TYPE focus fields, apart from the combat origin (TYPE+0x9c): the camera follows the
 * point they give and the rewards are thrown about it (legendsN64BossFocus). */
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

/** The point Boss.bin's e0884a9c publishes through e0038d90 on each frame a boss of `type`
 * stands on `floorAnchor`: its root, TYPE+0x94 over that floor, raised by TYPE+0x98
 * (e0884cfc-e0884d0c, e0884d60-e0884dac). e0889850 keeps it at e088f990, which e08898d8 throws
 * the rewards about. */
Vec3 legendsN64BossFocus(const LegendsBossCamera& type, const Vec3& floorAnchor);

} // namespace gdl::game
