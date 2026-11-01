#include "expansions/n64/scenes/LegendsN64Music.h"

#include <array>
#include <string>
#include <utility>

namespace gdl {

std::optional<u16> legendsN64SongForLevel(std::string_view directoryName) {
    std::string nameKey(directoryName);
    for (char& character : nameKey) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    // US game.bin e006ab40: realm table e006c360 (24-byte records), level
    // table e006c468 (40-byte records), signed song token at level+20.
    // PlaySong e000cf28 masks token&0x3ff. Names do not reliably match songs:
    // Ice2 uses Dungeon, Ice5/6 Mount5, and Town4 uses Townt4, not Town4.
    // These are ordinary-level defaults. Native Tower progression can override
    // its song during a cinematic; the exploratory map viewer has no cinematics.
    static constexpr std::array<std::pair<std::string_view, u16>, 35> kRoutes{{
        {"castle1", 6},  {"castle6", 7},  {"castle3", 8},  {"castle4", 9},  {"castle5", 10},
        {"mount1", 0},   {"mount2", 1},   {"mount3", 2},   {"mount4", 3},   {"mount5", 4},
        {"mount6", 5},   {"ice1", 19},    {"ice2", 7},     {"ice3", 26},    {"ice4", 29},
        {"ice5", 4},     {"ice6", 4},     {"secret2", 37}, {"secret3", 36}, {"secret4", 34},
        {"secret5", 34}, {"secret6", 38}, {"town1", 12},   {"town2", 13},   {"town3", 14},
        {"town4", 18},   {"town5", 33},   {"battle1", 22}, {"battle2", 24}, {"battle3", 25},
        {"hell1", 31},   {"hell2", 32},   {"boss1", 28},   {"boss2", 21},   {"tower2", 17},
    }};
    for (const auto& [name, song] : kRoutes) {
        if (nameKey == name) {
            return song;
        }
    }
    return std::nullopt;
}

} // namespace gdl
