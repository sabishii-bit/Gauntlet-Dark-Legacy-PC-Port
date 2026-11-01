#include "expansions/n64/formats/LegendsN64Text.h"

#include <map>

#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {
namespace {
void require(bool value, std::string_view message) {
    if (!value) {
        throw FormatError("N64 Legends text: " + std::string(message));
    }
}
void range(std::span<const u8> bytes, usize offset, usize size) {
    require(offset <= bytes.size() && size <= bytes.size() - offset, "truncated data");
}
std::string stringAt(std::span<const u8> bytes, usize at, usize limit, bool path = false) {
    std::string text;
    for (usize i = 0; i < limit; ++i) {
        range(bytes, at + i, 1);
        const u8 value = bytes[at + i];
        if (value == 0) {
            return text;
        }
        require(value == '\n' || value == '\r' || value == '\t' || (value >= 32 && value < 127),
                "unsupported text encoding");
        if (path) {
            require((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                        (value >= '0' && value <= '9') || value == '_',
                    "unsafe text bank path");
        }
        text.push_back(static_cast<char>(value));
    }
    throw FormatError("N64 Legends text: unterminated string");
}
struct Chunk {
    usize offset = 0;
    usize count = 0;
};
Chunk chunk(std::span<const u8> bytes, u32 tag, usize stride) {
    range(bytes, 0, 8);
    const usize directory = readU32BE(bytes, 0);
    const usize count = readU32BE(bytes, 4);
    require(directory >= 8 && count <= 32, "invalid directory");
    range(bytes, directory, count * 12);
    std::optional<Chunk> result;
    for (usize i = 0; i < count; ++i) {
        const usize at = directory + i * 12;
        if (readU32BE(bytes, at) != tag) {
            continue;
        }
        require(!result, "duplicate chunk");
        const usize offset = readU32BE(bytes, at + 4);
        const usize size = readU32BE(bytes, at + 8);
        require(size <= 65536 && offset >= 8 && offset <= directory &&
                    size <= (directory - offset) / stride,
                "invalid chunk extent");
        result = Chunk{offset, size};
    }
    require(result.has_value(), "missing chunk");
    return *result;
}
} // namespace

std::optional<LegendsN64ScrollContext> legendsN64ScrollContext(std::span<const u8> program,
                                                               std::string_view map) {
    constexpr usize kBase = 0xe0000400U;
    constexpr usize kRealms = 0xe006c360U - kBase;
    constexpr usize kLevels = 0xe006c468U - kBase;
    struct Realm {
        char code;
        std::string stem;
    };
    std::map<u32, Realm> names;
    bool terminated = false;
    for (usize at = kRealms; at < kLevels; at += 24) {
        range(program, at, 24);
        const u32 id = readU32BE(program, at);
        if (id == 16) {
            terminated = true;
            break;
        }
        const u32 address = readU32BE(program, at + 8);
        require(id < 16 && address >= kBase, "invalid realm");
        require(names
                    .emplace(id, Realm{static_cast<char>(program[at + 4]),
                                       stringAt(program, address - kBase, 33, true)})
                    .second,
                "duplicate realm");
    }
    require(terminated, "unterminated realm table");
    std::optional<LegendsN64ScrollContext> result;
    for (usize i = 0; i < 128; ++i) {
        const usize at = kLevels + i * 40;
        range(program, at, 40);
        const u32 realm = readU32BE(program, at);
        if (realm == 16) {
            return result;
        }
        const auto found = names.find(realm);
        const u32 address = readU32BE(program, at + 12);
        const u32 level = readU32BE(program, at + 4);
        require(found != names.end() && address >= kBase && level <= 10, "invalid level");
        if (toLowerAscii(found->second.stem + stringAt(program, address - kBase, 33, true)) ==
            toLowerAscii(map)) {
            // e006ab40 sets the native string kBase from levelRow+0x26. Advice
            // uses the internal level ordinal, not the directory's suffix.
            result =
                LegendsN64ScrollContext{found->second.code, level, readU16BE(program, at + 0x26)};
        }
    }
    throw FormatError("N64 Legends text: unterminated level table");
}

std::optional<LegendsN64StringReference> legendsN64StringReference(std::span<const u8> library,
                                                                   u32 identity) {
    const auto names = chunk(library, 0x4e414d45U, 1); // NAME
    const auto toc = chunk(library, 0x00544f43U, 12);
    // e00390cc/e003906c: inclusive string ID ranges and a file-relative bank name.
    std::optional<LegendsN64StringReference> result;
    u32 previous = 0;
    for (usize i = 0; i < toc.count; ++i) {
        const usize at = toc.offset + i * 12;
        const u32 first = readU16BE(library, at);
        const u32 last = readU16BE(library, at + 2);
        const usize name = readU32BE(library, at + 4);
        require(first > previous && last >= first, "invalid string ranges");
        previous = last;
        require(name >= names.offset && name < names.offset + names.count, "invalid bank name");
        auto bank = stringAt(library, name, names.offset + names.count - name, true);
        require(!bank.empty(), "empty bank name");
        if (identity >= first && identity <= last) {
            result = LegendsN64StringReference{std::move(bank), identity - first};
        }
    }
    return result;
}

std::vector<std::string> decodeLegendsN64Text(std::span<const u8> bytes) {
    const auto strings = chunk(bytes, 0x53545253U, 1); // STRS
    const auto toc = chunk(bytes, 0x00544f43U, 4);
    std::vector<std::string> result;
    result.reserve(toc.count);
    // e00391a8 relocates each TOC word against the complete WAD allocation.
    for (usize i = 0; i < toc.count; ++i) {
        const usize at = readU32BE(bytes, toc.offset + i * 4);
        require(at >= strings.offset && at < strings.offset + strings.count,
                "invalid string offset");
        result.push_back(stringAt(bytes, at, strings.offset + strings.count - at));
    }
    return result;
}
} // namespace gdl::formats
