#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/assets/StringTable.h"

namespace gdl {

/** One message of an unpacked text rom: its pages, drawn in one of the rom's fonts. */
struct MessageInfo {
    std::string name;
    std::int32_t font = 0; ///< index into the table's fonts
    float scale = 1.0f;
    float shadowScale = 1.0f;
    std::vector<std::string> pages; ///< each with its own line breaks
};

/** A named list of a rom's messages, such as the hints about each guardian in turn. */
struct MessageList {
    std::string name;
    std::vector<std::int32_t> messages; ///< indices into the table's messages
};

/** The messages of one unpacked text rom (`text/<name>.json`), such as the scroll texts. */
class MessageTable {
public:
    /** Reads the file; false (with a warning) when missing or malformed. */
    bool load(const std::filesystem::path& file);
    bool loaded() const { return !m_messages.empty(); }

    /** The string ids a message's pages go by: `<prefix>.<name in lower case>.<page from 1>`. */
    static std::string textId(std::string_view prefix, std::string_view name, std::size_t page);
    /** Takes each message's pages from `strings` when it has them (as many as it numbers from
     * one without a gap), so that the game's language is the string table's; the rom's own
     * text stays for a message it lacks. How many messages were taken from it. */
    std::size_t translate(const StringTable& strings, std::string_view prefix);
    std::size_t size() const { return m_messages.size(); }

    const std::vector<std::string>& fonts() const { return m_fonts; }
    const MessageInfo& message(std::uint32_t index) const;
    std::optional<std::uint32_t> find(std::string_view name) const;
    const std::vector<MessageList>& lists() const { return m_lists; }
    /** The list of that name, or null. */
    const MessageList* findList(std::string_view name) const;
    /** The message a list names at `entry`, or null past its end. */
    const MessageInfo* listed(const MessageList& list, std::size_t entry) const;
    /** The font a message draws with, empty when the table lacks it. */
    std::string_view fontOf(const MessageInfo& message) const;

private:
    std::vector<std::string> m_fonts;
    std::vector<MessageInfo> m_messages;
    std::vector<MessageList> m_lists;
    std::unordered_map<std::string, std::uint32_t> m_byName;
};

} // namespace gdl
