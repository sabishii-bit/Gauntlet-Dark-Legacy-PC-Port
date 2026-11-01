#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/core/Types.h"

namespace gdl {

/**
 * User-facing text by identifier, read from a language's JSON file so the game never holds
 * literal strings. A fallback language covers identifiers a translation lacks.
 */
// NOLINTNEXTLINE(bugprone-exception-escape): the map's move is not noexcept on every library
class StringTable {
public:
    /** A language a directory holds: its code (the file's name) and the name it calls itself
     * (`language.name`), the code again when it gives none. */
    struct Language {
        std::string code;
        std::string name;
        bool operator==(const Language&) const = default;
    };
    /** Every language with a table in `directory`, by code. */
    static std::vector<Language> available(const std::filesystem::path& directory);

    /** Loads `directory/<language>.json` over `directory/<fallback>.json`; false when neither
     * loads. */
    bool load(const std::filesystem::path& directory, std::string_view language,
              std::string_view fallback = "en");
    /** Lays the tables in `directory` over what is loaded: `<fallback>.json`, then the one
     * in this table's language, each where it exists; false when neither does. Their ids
     * replace any the table holds already. */
    bool layer(const std::filesystem::path& directory, std::string_view fallback = "en");

    bool loaded() const { return !m_entries.empty(); }
    usize size() const { return m_entries.size(); }
    const std::string& language() const { return m_language; }
    /** The languages the directory this loaded from holds. */
    std::vector<Language> languages() const { return available(m_directory); }

    /** SHA-256 of the effective language and ordered key/value bytes, including
     * fallback entries. File paths, JSON formatting and map iteration are not
     * part of the identity. This is a content check, not authentication. */
    std::string digest() const;

    /** The text for an identifier, or the identifier itself when it is unknown. */
    std::string_view get(std::string_view id) const;
    bool has(std::string_view id) const { return m_entries.contains(std::string(id)); }

private:
    bool merge(const std::filesystem::path& file);

    std::filesystem::path m_directory;
    std::string m_language;
    std::unordered_map<std::string, std::string> m_entries;
};

} // namespace gdl
