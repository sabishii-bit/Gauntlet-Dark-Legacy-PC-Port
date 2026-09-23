#include "engine/assets/StringTable.h"

#include <cstdint>
#include <exception>

#include <nlohmann/json.hpp>

#include "engine/core/Log.h"
#include "engine/io/File.h"

namespace gdl {

bool StringTable::load(const std::filesystem::path& directory, std::string_view language,
                       std::string_view fallback) {
    m_entries.clear();
    m_language.clear();
    bool any = false;
    if (fallback != language) {
        any = merge(directory / (std::string(fallback) + ".json"));
    }
    if (merge(directory / (std::string(language) + ".json"))) {
        any = true;
        m_language = language;
    } else if (any) {
        log::warn("Strings: language '{}' not found under {}; using '{}'", language,
                  directory.string(), fallback);
        m_language = fallback;
    }
    return any;
}

std::string_view StringTable::get(std::string_view id) const {
    const auto it = m_entries.find(std::string(id));
    return it == m_entries.end() ? id : std::string_view(it->second);
}

bool StringTable::merge(const std::filesystem::path& file) {
    try {
        const std::vector<std::uint8_t> bytes = readFile(file);
        const nlohmann::json root = nlohmann::json::parse(bytes.begin(), bytes.end());
        for (const auto& [key, value] : root.items()) {
            if (value.is_string()) {
                m_entries[key] = value.get<std::string>();
            }
        }
        return true;
    } catch (const std::exception& e) {
        log::warn("Strings {}: {}", file.string(), e.what());
        return false;
    }
}

} // namespace gdl
