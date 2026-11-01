#include "expansions/n64/formats/LegendsN64Portal.h"

#include <bit>
#include <cmath>
#include <map>

#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {
namespace {
constexpr u32 kProgramBase = 0xe0000400U;
constexpr usize kRealms = 0xe006c360U - kProgramBase;
constexpr usize kLevels = 0xe006c468U - kProgramBase;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw FormatError("N64 Legends portal: " + std::string(message));
    }
}
void range(std::span<const u8> bytes, usize offset, usize size) {
    require(offset <= bytes.size() && size <= bytes.size() - offset, "truncated native data");
}
std::string pathComponent(std::span<const u8> program, u32 address) {
    require(address >= kProgramBase, "invalid native map name address");
    const usize offset = address - kProgramBase;
    std::string result;
    for (usize i = 0; i <= 32; ++i) {
        range(program, offset + i, 1);
        const u8 value = program[offset + i];
        if (value == 0) {
            require(!result.empty(), "empty native map name");
            return result;
        }
        require((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
                    (value >= '0' && value <= '9') || value == '_',
                "unsafe native map name");
        result.push_back(static_cast<char>(value));
    }
    throw FormatError("N64 Legends portal: unterminated native map name");
}
std::string sequence(std::span<const u8> bytes, usize at) {
    std::string result;
    for (usize i = 0; i < 4; ++i) {
        require(bytes[at + i] >= 32 && bytes[at + i] < 127, "invalid exit sequence name");
        result.push_back(static_cast<char>(bytes[at + i]));
    }
    return result;
}
} // namespace

std::optional<std::string> legendsN64Destination(std::span<const u8> program, u32 destination) {
    if (destination == 0 || destination > 0xffffU) {
        return std::nullopt;
    }
    // e006bf34 translates the authored letter/digit into realm/level IDs.
    // e006ab40 then concatenates the realm stem and the level suffix. In
    // particular A6 is castle6, not ordinal1/"castle2"; Tower2 is T2.
    const u8 realmCode = static_cast<u8>((destination >> 8U) + 0x40U);
    const u8 levelCode = static_cast<u8>(destination + 0x30U);
    std::optional<u32> realm;
    std::string stem;
    bool terminated = false;
    for (usize at = kRealms; at < kLevels; at += 24) {
        range(program, at, 24);
        const u32 id = readU32BE(program, at);
        if (id == 16) {
            terminated = true;
            break;
        }
        require(id < 16, "invalid native realm table");
        if (!realm && program[at + 4] == realmCode) {
            realm = id;
            stem = pathComponent(program, readU32BE(program, at + 8));
        }
    }
    require(terminated, "unterminated native realm table");
    if (!realm) {
        return std::nullopt;
    }
    std::optional<std::string> result;
    for (usize i = 0; i < 128; ++i) {
        const usize at = kLevels + i * 40;
        range(program, at, 40);
        const u32 id = readU32BE(program, at);
        if (id == 16) {
            return result;
        }
        require(id < 16 && readU32BE(program, at + 4) <= 10, "invalid native level table");
        if (!result && id == *realm && program[at + 8] == levelCode) {
            result = stem + pathComponent(program, readU32BE(program, at + 12));
        }
    }
    throw FormatError("N64 Legends portal: unterminated native level table");
}

std::optional<std::string> legendsN64ExitDestination(std::span<const u8> program, u32 destination,
                                                     std::string_view sourceMap) {
    const auto targetName = legendsN64Destination(program, destination);
    std::map<u32, std::string> realms;
    bool terminated = false;
    for (usize at = kRealms; at < kLevels; at += 24) {
        range(program, at, 24);
        const u32 realm = readU32BE(program, at);
        if (realm == 16) {
            terminated = true;
            break;
        }
        require(realm < 16, "invalid native realm table");
        require(realms.emplace(realm, pathComponent(program, readU32BE(program, at + 8))).second,
                "duplicate native realm");
    }
    require(terminated, "unterminated native realm table");
    std::optional<u32> sourceRealm;
    std::optional<u32> targetRealm;
    u32 targetLevel = 0;
    std::optional<std::string> tower;
    terminated = false;
    const auto source = toLowerAscii(sourceMap);
    for (usize i = 0; i < 128; ++i) {
        const usize at = kLevels + i * 40;
        range(program, at, 40);
        const u32 realm = readU32BE(program, at);
        if (realm == 16) {
            terminated = true;
            break;
        }
        const u32 level = readU32BE(program, at + 4);
        const auto stem = realms.find(realm);
        require(stem != realms.end() && level <= 10, "invalid native level table");
        const auto name = stem->second + pathComponent(program, readU32BE(program, at + 12));
        if (toLowerAscii(name) == source) {
            sourceRealm = realm;
        }
        if (targetName && name == *targetName) {
            targetRealm = realm;
            targetLevel = level;
        }
        if (realm == 8 && level == 1) {
            tower = name;
        }
    }
    require(terminated, "unterminated native level table");
    if (!sourceRealm) {
        return std::nullopt;
    }
    // e006be0c, after e004a6e8 completes the player departure. A level's
    // encoded next-stage ID is not permission to bypass the retail Tower.
    const bool bossChain =
        targetRealm && (*targetRealm == 13 || *targetRealm == 5) && targetLevel == 8;
    const bool keepTarget =
        destination != 0 && ((targetRealm && *targetRealm == 14) || *sourceRealm == 8);
    return bossChain || keepTarget ? targetName : tower;
}

std::vector<LegendsN64Portal> decodeLegendsN64Portals(std::span<const u8> placements,
                                                      std::span<const u8> templates,
                                                      std::span<const u8> program,
                                                      std::string_view sourceMap) {
    if (placements.empty()) {
        return {};
    }
    range(placements, 0, 52);
    const usize count = readU16BE(placements, 42);
    if (count == 0) {
        return {};
    }
    const usize offset = readU32BE(placements, 12);
    require(offset >= 52, "trap placements overlap header");
    range(placements, offset, count * 24);
    range(templates, 0, 4);
    const usize templateCount = readU32BE(templates, 0);
    require(templateCount <= 4096, "trap template budget exceeded");
    range(templates, 4, templateCount * 96);
    std::map<u16, usize> keys;
    for (usize i = 0; i < templateCount; ++i) {
        const usize at = 4 + i * 96;
        require(keys.emplace(readU16BE(templates, at), at).second, "duplicate trap template");
    }
    std::vector<LegendsN64Portal> result;
    for (usize i = 0; i < count; ++i) {
        const usize at = offset + i * 24;
        const auto source = keys.find(readU16BE(placements, at + 8));
        require(source != keys.end(), "unknown trap template");
        const usize native = source->second;
        if ((readU16BE(templates, native + 2) & 15U) != 5) {
            continue;
        }
        require(readU32BE(templates, native + 32) == 1, "unsupported exit contact shape");
        LegendsN64Portal portal;
        portal.nativeIndex = static_cast<u32>(i);
        portal.destination = readU32BE(placements, at + 12);
        if (auto map = sourceMap.empty()
                           ? legendsN64Destination(program, portal.destination)
                           : legendsN64ExitDestination(program, portal.destination, sourceMap)) {
            portal.map = std::move(*map);
        }
        for (s32 axis = 0; axis < 3; ++axis) {
            portal.position[axis] = static_cast<f32>(static_cast<s16>(
                                        readU16BE(placements, at + static_cast<usize>(axis) * 2))) /
                                    4;
        }
        portal.yaw = static_cast<f32>(static_cast<s16>(readU16BE(placements, at + 6))) / 32;
        portal.scale = std::bit_cast<f32>(readU32BE(placements, at + 16));
        require(std::isfinite(portal.scale) && portal.scale <= 100, "invalid exit scale");
        // e00694e4: unset/nonpositive trap scale defaults to one.
        portal.scale = portal.scale > .001f ? portal.scale : 1;
        portal.radius = std::bit_cast<f32>(readU32BE(templates, native + 36)) * portal.scale;
        portal.height = std::bit_cast<f32>(readU32BE(templates, native + 40)) * portal.scale;
        require(std::isfinite(portal.radius) && portal.radius > 0 && portal.radius <= 10000 &&
                    std::isfinite(portal.height) && portal.height > 0 && portal.height <= 10000,
                "invalid exit contact size");
        portal.minimumPlayers = placements[at + 11];
        portal.placementFlags = placements[at + 10];
        portal.idle = sequence(templates, native + 12);
        portal.active = sequence(templates, native + 16);
        if (portal.active.back() == 'A' && portal.idle != portal.active) {
            portal.starting = portal.active;
            portal.starting.back() = 'S';
            portal.closing = portal.active;
            portal.closing.back() = 'E';
        }
        result.push_back(std::move(portal));
    }
    return result;
}

} // namespace gdl::formats
