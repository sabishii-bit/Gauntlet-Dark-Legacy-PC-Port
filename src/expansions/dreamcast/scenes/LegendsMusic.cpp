#include "expansions/dreamcast/scenes/LegendsMusic.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"
#include "engine/platform/Paths.h"

namespace gdl::game {
namespace {
void require(bool condition) {
    if (!condition) {
        throw FormatError("Dreamcast Legends music: invalid native worlddata");
    }
}

std::string lower(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return value;
}

std::string text(std::span<const u8> bytes, usize at, usize size) {
    require(at <= bytes.size() && size <= bytes.size() - at);
    usize count = 0;
    while (count < size && bytes[at + count] != 0) {
        ++count;
    }
    return lower(std::string(std::bit_cast<const char*>(bytes.data() + at), count));
}

std::vector<std::string> streamReferences(std::span<const u8> bytes, const std::string& level) {
    require(bytes.size() >= 8);
    const usize directory = readU32LE(bytes, 0);
    const usize count = readU32LE(bytes, 4);
    require(directory >= 8 && directory <= bytes.size() && count <= 4096 &&
            count <= (bytes.size() - directory) / 12);
    struct Section {
        usize offset;
        usize count;
    };
    std::map<u32, Section> sections;
    for (usize i = 0; i < count; ++i) {
        const usize at = directory + i * 12;
        Section section{readU32LE(bytes, at + 4), readU32LE(bytes, at + 8)};
        require(section.offset >= 8 && section.offset <= directory);
        require(sections.emplace(readU32LE(bytes, at), section).second);
    }
    const auto records = [&](u32 tag, usize stride) {
        const auto found = sections.find(tag);
        if (found == sections.end()) {
            return Section{0, 0};
        }
        const Section section = found->second;
        usize end = directory;
        for (const auto& [unused, other] : sections) {
            if (other.offset > section.offset) {
                end = std::min(end, other.offset);
            }
        }
        require(section.count <= (end - section.offset) / stride);
        return section;
    };
    const Section levels = records(0x4c45564cU, 144); // LEVL (native reversed fourcc)
    const Section audio = records(0x41554453U, 60);   // AUDS
    std::vector<std::string> result;
    for (usize i = 0; i < levels.count; ++i) {
        const usize at = levels.offset + i * 144;
        if (text(bytes, at + 8, 4) != level) {
            continue;
        }
        // Dreamcast LEVL omits GameCube's 16-byte movie field: the selected
        // audio record follows its six enemy indices at +0x3c, then camera +72.
        const s16 slot = std::bit_cast<s16>(readU16LE(bytes, at + 74));
        require(slot >= 0 && static_cast<usize>(slot) < audio.count);
        const usize sound = audio.offset + static_cast<usize>(slot) * 60;
        // Multiple music areas require the native zone/stream selector, which
        // is not part of static traversal. Do not substitute a guessed suffix.
        if (readU16LE(bytes, sound + 40) != 1) {
            continue;
        }
        const auto base = text(bytes, sound + 24, 16);
        if (!base.empty() && std::all_of(base.begin(), base.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            })) {
            result.push_back(base + ".str");
        }
    }
    return result;
}
} // namespace

std::optional<std::filesystem::path>
dreamcastLegendsMusic(const std::filesystem::path& levelDirectory) {
    auto directory = levelDirectory.lexically_normal();
    while (directory.has_relative_path() && directory.filename().empty()) {
        directory = directory.parent_path();
    }
    const std::string name = lower(paths::utf8(directory.filename()));
    if (name.size() != 7 || !name.starts_with("level") || name[5] < 'a' || name[5] > 'z' ||
        name[6] < '0' || name[6] > '9') {
        return std::nullopt;
    }
    const auto native = directory.parent_path().parent_path();
    const auto worlddata = native / "worlddata";
    const auto streams = native.parent_path() / "disc/AUDIO/STREAMS";
    if (!std::filesystem::is_directory(worlddata) || !std::filesystem::is_directory(streams)) {
        return std::nullopt;
    }
    std::map<std::string, std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(streams)) {
        if (entry.is_regular_file() && lower(paths::utf8(entry.path().extension())) == ".str") {
            require(
                files.emplace(lower(paths::utf8(entry.path().filename())), entry.path()).second);
        }
    }
    std::optional<std::filesystem::path> result;
    for (const auto& entry : std::filesystem::directory_iterator(worlddata)) {
        if (!entry.is_regular_file() || lower(paths::utf8(entry.path().extension())) != ".wad") {
            continue;
        }
        require(entry.file_size() <= std::uintmax_t{16} * 1024 * 1024);
        for (const auto& reference : streamReferences(readFile(entry.path()), name.substr(5))) {
            const auto found = files.find(reference);
            if (found == files.end()) {
                continue;
            }
            // Conflicting authored mappings are not resolved by directory order.
            if (result && *result != found->second) {
                return std::nullopt;
            }
            result = found->second;
        }
    }
    return result;
}

} // namespace gdl::game
