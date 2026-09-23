#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::formats {

struct TextMessage {
    std::string name;
    s32 font = 0; ///< index into TextRom::fonts
    f32 scale = 1.0f;
    f32 shadowScale = 1.0f;
    std::vector<std::string> lines;
};

struct TextMessageList {
    std::string name;
    std::vector<u32> messages; ///< indices into TextRom::messages
};

/** A string resource from the TEXT directory: font names, named messages and named message lists.
 */
struct TextRom {
    std::vector<std::string> fonts;
    std::vector<TextMessage> messages;
    std::vector<TextMessageList> lists;

    /** Parses the little-endian lump container; throws FormatError. */
    static TextRom parse(std::span<const u8> file);

    std::optional<usize> findMessage(std::string_view name) const;
    std::optional<usize> findList(std::string_view name) const;
};

} // namespace gdl::formats
