#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::formats {

/** One tagged section of a data wad: where its records start and how many there are. */
struct WadSection {
    std::string tag; ///< four characters, as the game names them
    u32 offset = 0;
    u32 count = 0;
};

/**
 * Reads the directory of a data wad (the PDATA and WDATA files): a little-endian header
 * {directory offset, section count} and sixteen-byte entries {tag stored reversed, offset,
 * count, count}. `what` names the file in errors. Throws FormatError.
 */
std::vector<WadSection> readWadDirectory(std::span<const u8> bytes, std::string_view what);

/** The section tagged `tag`, or null. */
const WadSection* findWadSection(std::span<const WadSection> sections, std::string_view tag);

/** A little-endian value inside a wad, with a bounds check that throws FormatError. */
u32 readWadU32(std::span<const u8> bytes, usize offset, std::string_view what);
u16 readWadU16(std::span<const u8> bytes, usize offset, std::string_view what);
f32 readWadF32(std::span<const u8> bytes, usize offset, std::string_view what);
/** A fixed-width text field, cut at its first zero. */
std::string readWadText(std::span<const u8> bytes, usize offset, usize width,
                        std::string_view what);

} // namespace gdl::formats
