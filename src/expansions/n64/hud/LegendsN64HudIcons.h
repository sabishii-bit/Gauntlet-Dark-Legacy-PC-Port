#pragma once

#include <array>
#include <optional>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::game {

/** The N64 bottle (POTIONR, POTIONB, POTIONY, POTIONG) Dark Legacy's potion kind `kind` shows
 * as: its kinds as StatusBoxPainter::kPotionIcons colours them, red for an empty stack's 0 and
 * any kind it does not know. */
std::string_view legendsN64PotionBottle(s32 kind);

/** The N64 bank of class `classIndex`'s portraits, `hud/warf` for the warrior; none for a class
 * the N64 has no portrait of. */
std::optional<std::string_view> legendsN64ClassBank(s32 classIndex);

/** The code that names class `classIndex`'s N64 portraits, `WAR` for `WAR_P1` and `WAR_P2`;
 * none for a class the N64 has no portrait of. */
std::optional<std::string_view> legendsN64ClassCode(s32 classIndex);

/** The english/game.wad string the N64 panel names class `classIndex` by, Pojo's for a
 * character carrying him; none for a class the N64 has no word for. */
std::optional<s32> legendsN64ClassText(s32 classIndex, bool pojo);

/** The HUD bank's icon (HUD_) the N64 carousel shows Dark Legacy's powerup of `kind` and
 * `flags` by, matched as Dark Legacy names its powerups: a pair that carries flags first, every
 * one of them held, gold invulnerability before silver; then a weapon's amulet by its element;
 * then the kind's own pair. None where the N64 has no icon for the powerup. */
std::optional<std::string_view> legendsN64PowerupIcon(s32 kind, u32 flags);

/** Dark Legacy's INVENTORY picture (SHP_) the carousel shows the powerup of `kind` and `flags`
 * by where the N64 has no icon for it; none for the rest, and for a pair it knows nothing of. */
std::optional<std::string_view> legendsN64PowerupFallback(s32 kind, u32 flags);

/** The realms whose bosses Dark Legacy's legend items are for, in the order the quest page
 * lists the items. */
inline constexpr std::array<s32, 9> kLegendsN64LegendRealms{1, 2, 3, 4, 5, 7, 9, 10, 11};

/** The HUD bank's icon of the legend item for realm `realm`'s boss; none where the N64 has
 * none. */
std::optional<std::string_view> legendsN64LegendIcon(s32 realm);

/** Dark Legacy's SELECT picture the quest page shows realm `realm`'s legend item by where the
 * N64 has no icon for it; none for the rest. */
std::optional<std::string_view> legendsN64LegendFallback(s32 realm);

/** Every HUD bank icon the carousel and the quest page may draw, each once. */
std::vector<std::string_view> legendsN64ItemIcons();

/** Every INVENTORY picture the carousel may borrow, each once. */
std::vector<std::string_view> legendsN64PowerupFallbacks();

} // namespace gdl::game
