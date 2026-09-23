#include "engine/io/AssetLocator.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <format>
#include <string>
#include <system_error>
#include <utility>

#include "engine/core/Error.h"

namespace gdl {

namespace {

bool equalsIgnoringCase(std::string_view a, std::string_view b) {
    return std::ranges::equal(
        a, b, [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

std::optional<std::filesystem::path> matchComponent(const std::filesystem::path& directory,
                                                    std::string_view component) {
    std::error_code ec;
    std::optional<std::filesystem::path> loose;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        const std::string name = entry.path().filename().string();
        if (name == component) {
            return entry.path();
        }
        if (!loose.has_value() && equalsIgnoringCase(name, component)) {
            loose = entry.path();
        }
    }
    return loose;
}

} // namespace

AssetLocator::AssetLocator(std::filesystem::path root) : m_root(std::move(root)) {}

std::optional<std::filesystem::path> AssetLocator::find(std::string_view relative) const {
    std::filesystem::path current = m_root;
    std::size_t start = 0;
    while (start <= relative.size()) {
        const std::size_t end = relative.find_first_of("/\\", start);
        const std::string_view component = relative.substr(
            start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!component.empty()) {
            const auto next = matchComponent(current, component);
            if (!next.has_value()) {
                return std::nullopt;
            }
            current = *next;
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return current;
}

std::filesystem::path AssetLocator::require(std::string_view relative) const {
    const auto found = find(relative);
    if (!found.has_value()) {
        throw FileError(std::format("asset {} not found under {}", relative, m_root.string()));
    }
    return *found;
}

} // namespace gdl
