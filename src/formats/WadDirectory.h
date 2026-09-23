#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gdl::formats {

/** One tagged section of a data wad: where its records start and how many there are. */
struct WadSection {
    std::string tag; ///< four characters, as the game names them
    std::uint32_t offset = 0;
    std::uint32_t count = 0;
};

/**
 * Reads the directory of a data wad (the PDATA and WDATA files): a little-endian header
 * {directory offset, section count} and sixteen-byte entries {tag stored reversed, offset,
 * count, count}. `what` names the file in errors. Throws FormatError.
 */
std::vector<WadSection> readWadDirectory(std::span<const std::uint8_t> bytes,
                                         std::string_view what);

/** The section tagged `tag`, or null. */
const WadSection* findWadSection(std::span<const WadSection> sections, std::string_view tag);

/** A little-endian value inside a wad, with a bounds check that throws FormatError. */
std::uint32_t readWadU32(std::span<const std::uint8_t> bytes, std::size_t offset,
                         std::string_view what);
std::uint16_t readWadU16(std::span<const std::uint8_t> bytes, std::size_t offset,
                         std::string_view what);
float readWadF32(std::span<const std::uint8_t> bytes, std::size_t offset, std::string_view what);
/** A fixed-width text field, cut at its first zero. */
std::string readWadText(std::span<const std::uint8_t> bytes, std::size_t offset, std::size_t width,
                        std::string_view what);

} // namespace gdl::formats
