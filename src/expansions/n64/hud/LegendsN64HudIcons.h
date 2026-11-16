#pragma once

#include <optional>
#include <string>
#include <string_view>

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

/** The text id of the N64 HUD's english/game.wad string `native`. */
std::string legendsN64HudTextKey(s32 native);

} // namespace gdl::game
