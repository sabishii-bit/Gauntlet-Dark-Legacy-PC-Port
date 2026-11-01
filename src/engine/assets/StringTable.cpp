#include "engine/assets/StringTable.h"

#include <algorithm>
#include <array>
#include <exception>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/io/Sha256.h"
#include "engine/platform/Paths.h"

namespace gdl {

bool StringTable::load(const std::filesystem::path& directory, std::string_view language,
                       std::string_view fallback) {
    m_entries.clear();
    m_language.clear();
    m_directory = directory;
    bool any = false;
    if (fallback != language) {
        any = merge(directory / (std::string(fallback) + ".json"));
    }
    if (merge(directory / (std::string(language) + ".json"))) {
        any = true;
        m_language = language;
    } else if (any) {
        log::warn("Strings: language '{}' not found under {}; using '{}'", language,
                  paths::utf8(directory), fallback);
        m_language = fallback;
    }
    return any;
}

bool StringTable::layer(const std::filesystem::path& directory, std::string_view fallback) {
    const auto present = [&](std::string_view language) {
        std::error_code error;
        return !language.empty() && std::filesystem::is_regular_file(
                                        directory / (std::string(language) + ".json"), error);
    };
    bool any = false;
    if (fallback != m_language && present(fallback)) {
        any = merge(directory / (std::string(fallback) + ".json"));
    }
    if (present(m_language)) {
        any = merge(directory / (m_language + ".json")) || any;
    }
    return any;
}

std::vector<StringTable::Language> StringTable::available(const std::filesystem::path& directory) {
    std::vector<Language> languages;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }
        try {
            const std::vector<u8> bytes = readFile(entry.path());
            const nlohmann::json root = nlohmann::json::parse(bytes.begin(), bytes.end());
            if (!root.is_object()) {
                continue;
            }
            Language language;
            language.code = paths::utf8(entry.path().stem());
            language.name = root.contains("language.name") && root.at("language.name").is_string()
                                ? root.at("language.name").get<std::string>()
                                : language.code;
            languages.push_back(std::move(language));
        } catch (const std::exception& e) {
            log::warn("Strings {}: {}", paths::utf8(entry.path()), e.what());
        }
    }
    std::ranges::sort(languages, {}, &Language::code);
    return languages;
}

std::string_view StringTable::get(std::string_view id) const {
    const auto it = m_entries.find(std::string(id));
    return it == m_entries.end() ? id : std::string_view(it->second);
}

std::string StringTable::digest() const {
    Sha256 hash;
    const auto word = [&](u64 value) {
        std::array<u8, 8> bytes{};
        for (usize i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<u8>(value >> (8 * i));
        }
        hash.update(bytes);
    };
    const auto text = [&](std::string_view value) {
        word(value.size());
        // A byte view of a trivially copyable buffer.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        hash.update({reinterpret_cast<const u8*>(value.data()), value.size()});
    };
    text("gdl-effective-string-table-v1");
    text(m_language);
    word(m_entries.size());
    std::vector<std::string_view> keys;
    keys.reserve(m_entries.size());
    for (const auto& [key, value] : m_entries) {
        (void)value;
        keys.push_back(key);
    }
    std::ranges::sort(keys);
    for (const auto key : keys) {
        text(key);
        text(get(key));
    }
    return hash.hex();
}

bool StringTable::merge(const std::filesystem::path& file) {
    try {
        const std::vector<u8> bytes = readFile(file);
        const nlohmann::json root = nlohmann::json::parse(bytes.begin(), bytes.end());
        for (const auto& [key, value] : root.items()) {
            if (value.is_string()) {
                m_entries[key] = value.get<std::string>();
            }
        }
        return true;
    } catch (const std::exception& e) {
        log::warn("Strings {}: {}", paths::utf8(file), e.what());
        return false;
    }
}

} // namespace gdl
