#pragma once

#include <filesystem>

#include "engine/assets/BitmapFont.h"
#include "engine/core/Types.h"

namespace gdl::game {

/** Where FONT32's hyphen is cut from: the crossbar of its E, a short brush stroke tapered
 * on the right, taken just clear of the stem and set at the middle of the capitals with a
 * little room either side. */
inline constexpr BitmapFont::Cut kMenuFontHyphen{.code = '-',
                                                 .from = 'E',
                                                 .left = 8,
                                                 .top = 10,
                                                 .columns = 11,
                                                 .rows = 4,
                                                 .width = 15,
                                                 .lead = 2,
                                                 .at = 12};

/** Loads FONT32, the brush-lettered menu and caption font, from `fonts/font32.json` (or
 * its native .fnt) under `unpackedRoot`, with the hyphen its artists never drew: the names
 * of powerups hold one. False when the font is not there. */
bool loadMenuFont(BitmapFont& font, const std::filesystem::path& unpackedRoot, s32 spaceWidth);

} // namespace gdl::game
