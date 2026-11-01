#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::game {

struct LegendsSoundEntry {
    u16 token = 0xffff;
    u16 replacement = 0xffff;
    u8 volume = 255;
    u8 pan = 128;
    f32 pitch = 1;
    std::string name;
};

/** Audited US game-side cue metadata, independent of the optional MusyX backend. */
std::vector<LegendsSoundEntry> decodeLegendsSoundTable(std::span<const u8> program);

/** e007a408 selects the first full case-insensitive name; absent names are silent. */
std::optional<u32> legendsSoundToken(std::span<const LegendsSoundEntry> entries,
                                     std::string_view name);

} // namespace gdl::game
