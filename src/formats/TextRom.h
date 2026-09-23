#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gdl::formats {

struct TextMessage {
    std::string name;
    std::int32_t font = 0; ///< index into TextRom::fonts
    float scale = 1.0f;
    float shadowScale = 1.0f;
    std::vector<std::string> lines;
};

struct TextMessageList {
    std::string name;
    std::vector<std::uint32_t> messages; ///< indices into TextRom::messages
};

/** A string resource from the TEXT directory: font names, named messages and named message lists.
 */
struct TextRom {
    std::vector<std::string> fonts;
    std::vector<TextMessage> messages;
    std::vector<TextMessageList> lists;

    /** Parses the little-endian lump container; throws FormatError. */
    static TextRom parse(std::span<const std::uint8_t> file);

    std::optional<std::size_t> findMessage(std::string_view name) const;
    std::optional<std::size_t> findList(std::string_view name) const;
};

} // namespace gdl::formats
