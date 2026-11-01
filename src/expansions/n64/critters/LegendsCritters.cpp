#include "expansions/n64/critters/LegendsCritters.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <format>
#include <map>
#include <optional>

#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/io/ByteReader.h"
#include "engine/math/PortableMath.h"

#include "expansions/n64/formats/LegendsN64Scene.h"
#include "game/enemies/EnemyKinds.h"
#include "game/enemies/EnemyMind.h"

namespace gdl::game {
namespace {
constexpr u32 kBase = 0xe0000400U;
void require(bool condition, std::string_view message) {
    if (!condition) {
        throw FormatError("N64 Legends actors: " + std::string(message));
    }
}
void range(std::span<const u8> bytes, usize at, usize size) {
    require(at <= bytes.size() && size <= bytes.size() - at, "truncated native data");
}
s16 signed16(std::span<const u8> bytes, usize at) {
    return std::bit_cast<s16>(readU16BE(bytes, at));
}
f32 real(std::span<const u8> bytes, usize at) {
    const f32 value = std::bit_cast<f32>(readU32BE(bytes, at));
    require(std::isfinite(value), "non-finite native value");
    return value;
}
bool finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
std::string nativeName(std::span<const u8> program, u32 address) {
    require(address >= kBase, "invalid native name pointer");
    const usize at = address - kBase;
    std::string name;
    for (usize i = 0; i < 32; ++i) {
        range(program, at + i, 1);
        const u8 c = program[at + i];
        if (c == 0) {
            require(!name.empty(), "empty native name");
            return name;
        }
        require((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '_',
                "invalid native name");
        name.push_back(static_cast<char>(c));
    }
    throw FormatError("N64 Legends actors: unterminated native name");
}
struct Roster {
    std::array<u8, 3> species{}; // medium, large, small, as e006a858 stores them
    char costume = 0;
};
std::optional<Roster> roster(std::span<const u8> program, std::string_view name) {
    if (program.empty()) {
        return std::nullopt;
    }
    struct Realm {
        std::string stem;
        std::array<u8, 3> species;
        char costume;
    };
    // e006a858 merges these tables. Read their actual level ordering/overrides,
    // not a second roster whose guessed map-prefix rules could drift.
    std::map<u32, Realm> realms;
    bool terminated = false;
    for (usize i = 0; i < 16; ++i) {
        const usize at = 0xe006c360U - kBase + i * 24;
        range(program, at, 24);
        const u32 id = readU32BE(program, at);
        if (id == 16) {
            terminated = true;
            break;
        }
        require(id < 16, "invalid realm identity");
        const char nativeCostume = static_cast<char>(program[at + 15]);
        // Explicit corresponding GDL realm costumes, as the port's wdata names
        // them. N64's C/M/T/B are not GDL's A/B/G/H and cannot be used directly.
        char costume = 0;
        switch (nativeCostume) {
        case 'C': costume = 'A'; break;
        case 'M': costume = 'B'; break;
        case 'I': costume = 'I'; break;
        case 'T': costume = 'G'; break;
        case 'B': costume = 'H'; break;
        case 'H': costume = 'F'; break;
        default: break;
        }
        require(realms
                    .emplace(id, Realm{nativeName(program, readU32BE(program, at + 8)),
                                       {program[at + 12], program[at + 13], program[at + 14]},
                                       costume})
                    .second,
                "duplicate realm identity");
    }
    require(terminated, "unterminated realm table");
    for (usize i = 0; i < 128; ++i) {
        const usize at = 0xe006c468U - kBase + i * 40;
        range(program, at, 40);
        const u32 id = readU32BE(program, at);
        if (id == 16) {
            return std::nullopt;
        }
        const auto found = realms.find(id);
        require(found != realms.end(), "unknown level realm");
        const auto& realm = found->second;
        if (toLowerAscii(realm.stem + nativeName(program, readU32BE(program, at + 12))) !=
            toLowerAscii(std::string(name))) {
            continue;
        }
        Roster result{realm.species, realm.costume};
        for (usize j = 0; j < 3; ++j) {
            if (program[at + 16 + j] != 0x22) {
                result.species[j] = program[at + 16 + j];
            }
        }
        return result;
    }
    throw FormatError("N64 Legends actors: unterminated level table");
}
std::optional<s32> commonKind(u8 species) {
    // Native species archive table e006c0c4; same names as LegendsGenerators.
    switch (species) {
    case 0: return 0;
    case 1: return 1;
    case 2: return 2;
    case 3: return 3;
    case 4: return 4;
    case 5: return 5;
    case 12: return 16;
    case 13: return 17;
    case 14: return 15;
    case 15: return 13;
    case 16: return 12;
    case 17: return 14;
    case 18: return 19;
    case 19: return 18;
    default: return std::nullopt;
    }
}
std::optional<s32> commonAlgorithm(u8 algorithm) {
    // Native table e005fcdc: 15 is e005e40c -> chase7, NOT GDL patrol15.
    // 16/23 share ranged e005e438 with mobile/stationary parameter. 17/26
    // alias16 (e005e524); tier5 chooses bombs. 18 is suicide e005e550.
    switch (algorithm) {
    case 15: return kChaseWay;
    case 16: return kSkirmishWay;
    case 17: return kSkirmishBombWay;
    case 18: return kSuicideWay;
    case 23: return kThrowWay;
    case 26: return kSkirmishBombWay;
    default: return std::nullopt;
    }
}
} // namespace

LegendsActorRoster
decodeLegendsN64Actors(std::span<const u8> placements, std::span<const u8> templates,
                       std::span<const u8> program, std::string_view levelName,
                       std::span<const formats::LegendsMapCollisionTriangle> collision) {
    range(placements, 0, 92);
    LegendsActorRoster result;
    const usize count = readU16BE(placements, 40);
    if (count == 0) {
        return result;
    }
    const usize offset = readU32BE(placements, 8);
    require(offset >= 92 && count <= 16384, "invalid placement table");
    range(placements, offset, count * 16);
    range(templates, 0, 4);
    const usize templateCount = readU32BE(templates, 0);
    require(templateCount <= 4096, "template budget exceeded");
    range(templates, 4, templateCount * 56);
    std::map<std::pair<u16, u8>, usize> lookup;
    for (usize i = 0; i < templateCount; ++i) {
        const usize at = 4 + i * 56;
        require(lookup.emplace(std::pair{readU16BE(templates, at), templates[at + 10]}, at).second,
                "duplicate generator template");
    }
    std::optional<Roster> level;
    bool rosterRead = false;
    for (usize i = 0; i < count; ++i) {
        const usize at = offset + i * 16;
        if ((placements[at + 13] & 2U) == 0) {
            continue;
        }
        const auto reject = [&](std::string reason) {
            result.warnings.push_back(std::format("Native actor {}: {}", i, reason));
        };
        const u16 key = readU16BE(placements, at + 8);
        const u8 tier = placements[at + 10];
        const auto found = lookup.find({key, tier});
        require(found != lookup.end(), "actor has no matching template");
        const usize native = found->second;
        require(placements[at + 14] <= 4, "invalid player count");
        if (placements[at + 13] != 3 || placements[at + 12] != 0 || placements[at + 15] != 0 ||
            readU32BE(templates, native + 4) != 0x01000000U ||
            readU32BE(templates, native + 28) != 5 || tier == 0 || tier > 6) {
            reject("unsupported activation or pending body");
            continue;
        }
        if (!rosterRead) {
            level = roster(program, levelName);
            rosterRead = true;
        }
        if (!level) {
            reject("native level roster unavailable");
            continue;
        }
        const u8 kind = templates[native + 9];
        std::optional<s32> mapped;
        std::optional<s32> algorithm;
        if (kind == 6) {
            mapped = kGolemEnemyKind;
            algorithm = 19;
        } else if (kind == 9) {
            mapped = kItKind;
            algorithm = kLurkWay;
        } else if (kind == 1 || kind == 2) {
            const u8 species = level->species[kind == 1 ? 2 : tier < 4 ? 0 : 1];
            mapped = commonKind(species);
            algorithm = commonAlgorithm(placements[at + 11]);
            if (!mapped) {
                reject(std::format("native species {} has no fixed GDL counterpart", species));
                continue;
            }
        }
        if (!mapped || !algorithm) {
            reject("unsupported native class or strategy");
            continue;
        }
        ImportedActor actor;
        actor.nativeIndex = static_cast<u32>(i);
        actor.spawn.kind = *mapped;
        actor.spawn.tier = tier;
        actor.spawn.algorithm = *algorithm;
        actor.minimumPlayers = placements[at + 14];
        actor.costumeRealm = level->costume;
        actor.activationRadius = real(templates, native + 44);
        actor.activationHeight = real(templates, native + 48);
        require(actor.activationRadius > 0 && actor.activationRadius <= 1024 &&
                    actor.activationHeight >= 0 && actor.activationHeight <= 1024,
                "invalid actor activation range");
        formats::LegendsN64Fixture fixture;
        fixture.kind = formats::LegendsN64FixtureKind::Generator;
        fixture.position = Vec3{signed16(placements, at), signed16(placements, at + 2),
                                signed16(placements, at + 4)} /
                           4.0f;
        const auto floor = formats::legendsN64FixturePlacement(fixture, collision);
        actor.spawn.position = floor.position;
        actor.floorObject = floor.floorObject;
        actor.idleYaw = static_cast<f32>(signed16(placements, at + 6)) / 32;
        // e006760c applies -IPR yaw to the native clockwise tree transform;
        // e005ac34 resets golem/It live yaw to zero. Common bodies use CCW yaw.
        actor.yaw = kind == 6 || kind == 9 ? 0 : actor.idleYaw;
        actor.spawn.direction = {portable::sin(actor.yaw), 0, portable::cos(actor.yaw)};
        actor.spawn.placed = true;
        actor.spawn.priority = EnemySpawn::Priority::Visible;
        result.actors.push_back(actor);
    }
    return result;
}

bool legendsActorReady(const ImportedActor& actor, std::span<const EnemyView> players,
                       bool timeStopped) {
    if (timeStopped || !finite(actor.spawn.position) || !std::isfinite(actor.activationRadius) ||
        actor.activationRadius <= 0 || !std::isfinite(actor.activationHeight) ||
        actor.activationHeight < 0) {
        return false;
    }
    // e0063618 compares actual active-player feet. Hidden/decoy/invisible
    // targeting flags do not participate in this physical proximity query.
    for (const auto& player : players) {
        if (player.player < 0 || player.player >= 4 || !finite(player.position)) {
            continue;
        }
        Vec3 delta = player.position - actor.spawn.position;
        if (actor.activationHeight != 0) {
            if (std::abs(delta.y) >= actor.activationHeight) {
                continue;
            }
            delta.y = 0;
        }
        if (glm::dot(delta, delta) <= actor.activationRadius * actor.activationRadius) {
            return true;
        }
    }
    return false;
}

} // namespace gdl::game
