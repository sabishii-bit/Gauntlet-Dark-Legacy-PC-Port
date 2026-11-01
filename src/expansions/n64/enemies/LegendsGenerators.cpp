#include "expansions/n64/enemies/LegendsGenerators.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <vector>

#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/io/ByteReader.h"

#include "game/enemies/EnemyKinds.h"

namespace gdl::game {
namespace {

struct Roster {
    std::string_view level;
    s32 medium;
    s32 small;
    u8 healthBand;
};

// US game.bin e006a858 merges realm e006c360 +12/+14 with level
// e006c468 +16/+18 (0x22 inherits; 0x23 means absent). e0067818 applies
// that result to PGEn class 2/1. These are N64 species IDs, not GDL IDs.
constexpr std::array kRosters{
    Roster{"mount1", 1, 35, 1},   Roster{"mount2", 1, 0, 1},    Roster{"mount3", 1, 0, 1},
    Roster{"mount4", 2, 0, 1},    Roster{"mount5", 2, 0, 1},    Roster{"castle1", 4, 3, 2},
    Roster{"castle6", 4, 3, 2},   Roster{"castle3", 5, 3, 2},   Roster{"castle4", 5, 3, 2},
    Roster{"ice1", 12, 14, 4},    Roster{"ice2", 13, 14, 4},    Roster{"ice3", 12, 14, 4},
    Roster{"ice4", 12, 14, 4},    Roster{"ice5", 12, 14, 4},    Roster{"town1", 15, 16, 3},
    Roster{"town2", 15, 16, 3},   Roster{"town3", 17, 16, 3},   Roster{"town4", 17, 16, 3},
    Roster{"battle1", 18, 19, 6}, Roster{"battle2", 18, 19, 6}, Roster{"battle3", 18, 19, 6},
    Roster{"hell1", 33, 35, 7},   Roster{"boss1", 32, 35, 5},
};

// e006a640's archive routing table e006c0c4 names these species explicitly.
// skel/sked are the skeleton and skeletal dog, GDL's SKE/DOG counterparts.
std::optional<s32> commonKind(s32 species) {
    switch (species) {
    case 0: return 0;   // sco
    case 1: return 1;   // tro
    case 2: return 2;   // dem
    case 3: return 3;   // rat
    case 4: return 4;   // gru
    case 5: return 5;   // kni
    case 12: return 16; // ice
    case 13: return 17; // worm
    case 14: return 15; // wolf
    case 15: return 13; // zom
    case 16: return 12; // mag
    case 17: return 14; // plague
    case 18: return 19; // skel
    case 19: return 18; // sked
    default: return std::nullopt;
    }
}

bool finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

std::string sequence(std::span<const u8> record) {
    if (readU32BE(record, 12) == 0) {
        return {};
    }
    std::string result = "GENERATOR:";
    for (usize i = 12; i < 16; ++i) {
        if (record[i] < 0x20 || record[i] > 0x7e) {
            throw FormatError("N64 generator has an invalid model identifier");
        }
        result += static_cast<char>(record[i]);
    }
    return result;
}

} // namespace

std::vector<std::string> legendsN64GeneratorArchives(std::string_view directory) {
    const auto roster = std::ranges::find(kRosters, toLowerAscii(directory), &Roster::level);
    if (roster == kRosters.end())
        return {};
    // e006a640 loads these medium/small species archives; their GENERATOR
    // groups supply the models used by e0067818. Town/Castle/Battle's item
    // archives do not contain those groups at all. Keep the verified loading
    // scope, not a global search for any tree with the same generic name.
    std::vector<std::string> result;
    for (const s32 species : {roster->medium, roster->small}) {
        switch (species) {
        case 3: result.emplace_back("rat"); break;
        case 4: result.emplace_back("gru"); break;
        case 5: result.emplace_back("kni"); break;
        case 15: result.emplace_back("zom"); break;
        case 16: result.emplace_back("mag"); break;
        case 17: result.emplace_back("plague"); break;
        case 18: result.emplace_back("skel"); break;
        case 19: result.emplace_back("sked"); break;
        default: break;
        }
    }
    return result;
}

std::optional<LegendsGenerator>
legendsN64Generator(const formats::LegendsN64Fixture& fixture, std::span<const u8> templates,
                    std::string_view directory,
                    std::span<const formats::LegendsMapCollisionTriangle> collision,
                    std::string* unsupported) {
    if (unsupported != nullptr) {
        unsupported->clear();
    }
    const auto reject = [&](std::string reason) -> std::optional<LegendsGenerator> {
        if (unsupported != nullptr) {
            *unsupported = std::move(reason);
        }
        return std::nullopt;
    };
    if (fixture.kind != formats::LegendsN64FixtureKind::Generator) {
        return std::nullopt;
    }
    if (fixture.actorInsteadOfGenerator || (fixture.templateKey >> 8U) != 6) {
        return reject("N64 placed-enemy templates are not repeating generators");
    }
    if (fixture.generatorTier == 0) {
        return std::nullopt; // a ruin never acquires a live GDL brood
    }
    if (fixture.generatorTier > 3 || fixture.minimumPlayers > 4 || !finite(fixture.position) ||
        !std::isfinite(fixture.facingRadians) || fixture.scale != 1.0f) {
        return reject("N64 generator has unsupported placement parameters");
    }
    if (templates.size() < 4 || readU32BE(templates, 0) > (templates.size() - 4) / 56) {
        throw FormatError("N64 generator template table is truncated");
    }
    const usize count = readU32BE(templates, 0);
    const auto find = [&](u16 key, u8 tier) -> std::span<const u8> {
        for (usize i = 0; i < count; ++i) {
            const auto record = templates.subspan(4 + i * 56, 56);
            if (readU16BE(record, 0) == key && record[10] == tier) {
                return record;
            }
        }
        return {};
    };
    auto record = find(fixture.templateKey, fixture.generatorTier);
    if (record.empty()) {
        throw FormatError("N64 generator placement has no matching template");
    }
    std::string name(directory);
    for (char& ch : name) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    // MSVC's checked array iterator is not a pointer; keep the portable iterator type.
    // NOLINTNEXTLINE(readability-qualified-auto)
    const auto roster = std::ranges::find(kRosters, name, &Roster::level);
    if (roster == kRosters.end()) {
        return reject("N64 generator level has no verified enemy roster");
    }
    const s32 species = record[9] == 1 ? roster->small : roster->medium;
    const bool mixed =
        record[9] == 10 && fixture.templateKey == 0x0663 && (species == 32 || species == 33);
    if (record[9] != 1 && record[9] != 2 && !mixed) {
        return reject("N64 mixed or special generator classes are not supported");
    }
    // e005ac34 class10 selects floor(random15*3/32768), always at tier3.
    // Tables e005fb98/e005fba4 contain DEM/KNI/ZOM and DEM/SKE/ZOM,
    // respectively. e006a858 loads these banks for realm species20/21.
    const std::array<s32, 3> mixedKinds{2, species == 32 ? 5 : 19, 13};
    const auto kind = mixed ? std::optional<s32>{mixedKinds.front()} : commonKind(species);
    if (!kind) {
        return reject(
            std::format("N64 generator species {} has no mapped GDL counterpart", species));
    }
    // e0067818's species→PGEn key table at e0067fb8. Skeleton variants have
    // no replacement entry and deliberately retain the original generator art.
    constexpr std::array<u8, 20> kKeys{2,  3,  4,  6,  5,  7,  9,  10, 11, 12,
                                       13, 14, 19, 20, 21, 16, 18, 17, 0,  0};
    if (!mixed && static_cast<usize>(species) < kKeys.size()) {
        const u16 remapped = static_cast<u16>(0x0600U | kKeys[static_cast<usize>(species)]);
        if (const auto replacement = find(remapped, fixture.generatorTier); !replacement.empty()) {
            record = replacement;
        }
    }
    const u32 contact = readU32BE(record, 28);
    const f32 across = std::bit_cast<f32>(readU32BE(record, 32));
    const f32 height = std::bit_cast<f32>(readU32BE(record, 36));
    const f32 along = std::bit_cast<f32>(readU32BE(record, 40));
    if ((contact != 1 && contact != 3) || !std::isfinite(across) || !std::isfinite(height) ||
        !std::isfinite(along) || across <= 0 || across > 32 || height <= 0 || height > 64 ||
        along < 0 || along > 32 || (contact == 3 && along == 0)) {
        return reject("N64 generator has unsupported contact geometry");
    }
    LegendsGenerator result;
    auto& out = result.gameplay;
    out.nativeIndex = fixture.nativeIndex;
    out.kind = *kind;
    out.tier = fixture.generatorTier;
    out.position = formats::legendsN64FixturePosition(fixture, collision);
    out.yaw = fixture.facingRadians;
    out.minimumPlayers = fixture.minimumPlayers;
    if (mixed) {
        out.broodKinds = mixedKinds;
        out.broodTier = 3;
    }
    auto& body = out.body;
    body.type = ItemInfo::kGenerator;
    body.name = enemyKind(*kind).name;
    // e006760c→e00638b8: cylinder(radius,height) or yaw box(across,height,along).
    body.radius = across;
    body.height = height;
    body.xSize = across;
    body.zSize = contact == 3 ? along : across;
    body.collisionType = static_cast<s32>(contact);
    body.collisionFlags = 1; // already projected by the native -10/+3 floor query
    // e004161c: stage table e0045ca8+3 selects 1+(band-1)/8. e0067f20
    // truncates each state's byte8 separately, not the sum of all three.
    const f32 healthScale = 1.0f + static_cast<f32>(roster->healthBand - 1) / 8.0f;
    // e0066ee8 subtracts physical+elemental integer hit points without armor.
    // Brood/attacks/rewards still use GDL mechanics, not its stock hut durability.
    body.armor = 0;
    const u16 resolvedKey = readU16BE(record, 0);
    std::vector<std::span<const u8>> liveStates;
    std::array<bool, 256> visited{};
    // State identifiers are not remaining durability or enemy tiers: the
    // special portal uses GSP1 -> GSP2 -> GSP0. Follow byte11, then index common
    // damage presentation by the number of live stages remaining.
    while (!record.empty() && record[10] != 0) {
        const u8 state = record[10];
        if (visited[state] || liveStates.size() == 3 ||
            (!mixed && record[11] != 0xff && record[11] != state - 1)) {
            return reject("N64 generator has a non-linear damage-state sequence");
        }
        visited[state] = true;
        if (record[8] == 0 || (readU32BE(record, 4) & 0x40000000U) != 0) {
            return reject("N64 generator has unsupported live-state durability");
        }
        liveStates.push_back(record);
        if (record[11] == 0xff) {
            record = {};
            break;
        }
        record = find(resolvedKey, record[11]);
        if (record.empty()) {
            return reject("N64 generator has a non-linear damage-state sequence");
        }
    }
    out.tier = static_cast<s32>(liveStates.size());
    if (out.tier == 0) {
        return reject("N64 generator has no live damage state");
    }
    if (!record.empty()) {
        result.sequences[0] = sequence(record);
    }
    for (usize i = 0; i < liveStates.size(); ++i) {
        const usize state = liveStates.size() - i;
        result.sequences[state] = sequence(liveStates[i]);
        const auto nativeHealth =
            static_cast<s32>(static_cast<f32>(liveStates[i][8]) * healthScale);
        // Imported levels retain GDL player combat rather than N64 class damage.
        // Deliberate port balance: halve each realm-scaled live stage, rounding
        // up and retaining at least one HP. This is not a retail durability claim.
        out.stageHealth[state - 1] = std::max(1, (nativeHealth + 1) / 2);
    }
    if (result.sequences[static_cast<usize>(out.tier)].empty()) {
        return reject("N64 generator has no live native artwork");
    }
    body.hitPoints = out.stageHealth.front();
    return result;
}

} // namespace gdl::game
