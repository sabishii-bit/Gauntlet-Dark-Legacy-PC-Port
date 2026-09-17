#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/core/Types.h"

namespace gdl {

/** One message of an unpacked text rom: its pages, drawn in one of the rom's fonts. */
struct MessageInfo {
    std::string name;
    s32 font = 0; ///< index into the table's fonts
    f32 scale = 1.0f;
    f32 shadowScale = 1.0f;
    std::vector<std::string> pages; ///< each with its own line breaks
};

/** The messages of one unpacked text rom (`text/<name>.json`), such as the scroll texts. */
class MessageTable {
public:
    /** Reads the file; false (with a warning) when missing or malformed. */
    bool load(const std::filesystem::path& file);
    bool loaded() const { return !m_messages.empty(); }
    usize size() const { return m_messages.size(); }

    const std::vector<std::string>& fonts() const { return m_fonts; }
    const MessageInfo& message(u32 index) const;
    std::optional<u32> find(std::string_view name) const;
    /** The font a message draws with, empty when the table lacks it. */
    std::string_view fontOf(const MessageInfo& message) const;

private:
    std::vector<std::string> m_fonts;
    std::vector<MessageInfo> m_messages;
    std::unordered_map<std::string, u32> m_byName;
};

} // namespace gdl
