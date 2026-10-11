#include "game/netplay/AssetDigest.h"

#include <array>
#include <map>
#include <stdexcept>

#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/io/Sha256.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gdl::game {
namespace {
void number(Sha256& hash, u64 value, usize width) {
    std::array<u8, 8> bytes{};
    for (usize i = 0; i < width; ++i) {
        bytes[i] = static_cast<u8>(value >> (i * 8));
    }
    hash.update(std::span(bytes).first(width));
}
void check(const std::stop_token& stop) {
    if (stop.stop_requested()) {
        throw std::runtime_error("Asset check cancelled");
    }
}
} // namespace
std::string assetDigest(const std::filesystem::path& root, const std::stop_token& stop) {
    std::map<std::string, std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        check(stop);
        bool linked = entry.is_symlink();
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(entry.path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            throw std::runtime_error("Cannot inspect native asset");
        }
        linked |= (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#endif
        if (linked) {
            throw std::runtime_error("Linked assets are not supported for online play");
        }
        if (entry.is_directory()) {
            continue;
        }
        if (!entry.is_regular_file()) {
            throw std::runtime_error("Unsupported native asset file");
        }
        auto name = entry.path().lexically_relative(root).generic_string();
        for (auto& c : name) {
            // Retail paths are ASCII. Reject non-ASCII mods instead of applying a different
            // locale-specific fold from the launcher's Unicode casefold.
            if (static_cast<u8>(c) >= 128) {
                throw std::runtime_error("Online asset paths must use ASCII names");
            }
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c + ('a' - 'A'));
            }
        }
        if (!files.emplace(name, entry.path()).second) {
            throw std::runtime_error("Ambiguous native asset casing");
        }
    }
    if (files.empty()) {
        throw std::runtime_error("No native assets found");
    }
    Sha256 digest;
    constexpr std::string_view kPrefix = "gdl-native-tree-v1";
    digest.update(std::vector<u8>(kPrefix.begin(), kPrefix.end()));
    digest.update(std::array<u8, 1>{0});
    std::vector<u8> buffer(usize{1024} * 1024);
    for (const auto& [name, path] : files) {
        check(stop);
        const auto size = std::filesystem::file_size(path);
        const auto time = std::filesystem::last_write_time(path);
        FileStream file(path);
        Sha256 content;
        while (const auto count = file.read(buffer)) {
            check(stop);
            content.update(std::span(buffer).first(count));
        }
        if (size != std::filesystem::file_size(path) ||
            time != std::filesystem::last_write_time(path)) {
            throw std::runtime_error("Native assets changed during verification");
        }
        number(digest, name.size(), 4);
        digest.update(std::vector<u8>(name.begin(), name.end()));
        number(digest, size, 8);
        digest.update(content.finish());
    }
    return digest.hex();
}
} // namespace gdl::game
