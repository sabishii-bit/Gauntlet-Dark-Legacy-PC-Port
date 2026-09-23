#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

#include "engine/core/Types.h"

namespace gdl {

/**
 * User-facing text by identifier, read from a language's JSON file so the game never holds
 * literal strings. A fallback language covers identifiers a translation lacks.
 */
class StringTable {
public:
    /** Loads `directory/<language>.json` over `directory/<fallback>.json`; false when neither
     * loads. */
    bool load(const std::filesystem::path& directory, std::string_view language,
              std::string_view fallback = "en");

    bool loaded() const { return !m_entries.empty(); }
    usize size() const { return m_entries.size(); }
    const std::string& language() const { return m_language; }

    /** The text for an identifier, or the identifier itself when it is unknown. */
    std::string_view get(std::string_view id) const;
    bool has(std::string_view id) const { return m_entries.contains(std::string(id)); }

private:
    bool merge(const std::filesystem::path& file);

    std::string m_language;
    std::unordered_map<std::string, std::string> m_entries;
};

} // namespace gdl
