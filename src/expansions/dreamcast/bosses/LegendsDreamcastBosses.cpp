#include "expansions/dreamcast/bosses/LegendsDreamcastBosses.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/io/ByteReader.h"

#include "formats/WorldFile.h"
#include "game/level/BossKinds.h"

namespace gdl::game {
namespace {

struct BossBinding {
    std::string_view level;
    s32 realm;
    s32 nativeKind;
    s32 kind;
    std::string_view archive;
    std::string_view world;
    std::string_view sound;
};

// LoadWorld 8c0a3580 dispatches ENMY IDs 26..33 to these native critter
// archives. Only the six arenas present in the shipped Dreamcast campaign
// are admitted here; unused PBOSS/YETI metadata is not substitute artwork.
constexpr std::array kBosses{
    BossBinding{"b6", 2, 26, boss_type::kDragon, "dragon", "MOUNT", "DRAGON"},
    BossBinding{"a5", 1, 27, boss_type::kChimera, "chimera", "CASTLE", "CHIMERA"},
    BossBinding{"c5", 3, 28, boss_type::kDjinn, "djinn", "DESERT", "DJINN"},
    BossBinding{"d5", 4, 29, boss_type::kDrider, "drider", "FOREST", "DRIDER"},
    BossBinding{"e2", 5, 32, boss_type::kSkorne1, "skorne1", "TEMPLE", "SKORNE1"},
    BossBinding{"f2", 6, 33, boss_type::kSkorne2, "skorne2", "HELL", "SKORNE2"},
};

void require(bool valid, const char* message) {
    if (!valid) {
        throw FormatError(std::string{"Dreamcast boss: "} + message);
    }
}

bool finite(const Vec3& point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

/** The shipped arena a lower-case level code names, or null for any other stage. */
const BossBinding* bindingOf(std::string_view level) {
    for (const auto& binding : kBosses) {
        if (binding.level == level) {
            return &binding;
        }
    }
    return nullptr;
}

} // namespace

std::optional<LegendsDreamcastBossEncounter> legendsDreamcastBoss(std::span<const u8> worlddata,
                                                                  std::span<const u8> worlds,
                                                                  std::string_view levelName) {
    std::string level = toLowerAscii(levelName);
    if (level.starts_with("level")) {
        level.erase(0, 5);
    }
    const auto* binding = bindingOf(level);
    if (binding == nullptr) {
        return std::nullopt;
    }
    require(worlddata.size() >= 8, "truncated worlddata header");
    const usize directory = readU32LE(worlddata, 0);
    const usize count = readU32LE(worlddata, 4);
    require(directory >= 8 && directory <= worlddata.size() && count <= 4096 &&
                count <= (worlddata.size() - directory) / 12,
            "invalid worlddata directory");
    struct Section {
        usize offset;
        usize count;
    };
    std::map<u32, Section> sections;
    for (usize index = 0; index < count; ++index) {
        const usize at = directory + index * 12;
        const Section section{readU32LE(worlddata, at + 4), readU32LE(worlddata, at + 8)};
        require(section.offset >= 8 && section.offset <= directory && section.count <= 4096,
                "invalid worlddata section");
        require(sections.emplace(readU32LE(worlddata, at), section).second,
                "duplicate worlddata section");
    }
    const auto records = [&](u32 tag, usize stride) {
        const auto found = sections.find(tag);
        require(found != sections.end(), "missing worlddata records");
        const Section section = found->second;
        usize end = directory;
        for (const auto& [unused, next] : sections) {
            if (next.offset > section.offset) {
                end = std::min(end, next.offset);
            }
        }
        require(section.count <= (end - section.offset) / stride,
                "worlddata records exceed section");
        return section;
    };
    const auto realm = records(0x57524c44U, 56);
    const auto levels = records(0x4c45564cU, 144);
    require(realm.count == 1, "worlddata requires one realm");
    const s32 nativeRealm = readS32LE(worlddata, realm.offset);
    for (usize row = 0; row < levels.count; ++row) {
        const usize at = levels.offset + row * 144;
        std::string code;
        for (usize character = 0; character < 4 && worlddata[at + 8 + character] != 0;
             ++character) {
            code += static_cast<char>(worlddata[at + 8 + character]);
        }
        if (toLowerAscii(code) != level) {
            continue;
        }
        const s32 nativeKind = readS32LE(worlddata, at + 52);
        if (nativeKind < 0) {
            return std::nullopt;
        }
        require(nativeRealm == binding->realm && nativeKind == binding->nativeKind,
                "arena identity disagrees with native campaign");

        // SetLocators 8c0a78a0 case 6 uses the authored Boss marker. 8c071120
        // passes that transform to CritterNew(4,0), not TYPE.defaultPos (which
        // frequently contains a 999-height roaming sentinel). Every shipped
        // boss marker is yaw-only; CreateYPRMatrix 8c0ed040 stores forward
        // (cos(pitch)*sin(yaw), sin(pitch), cos(pitch)*cos(yaw)).
        const auto world = formats::WorldFile::parse(worlds);
        const auto marker = std::ranges::find(world.locators, LocatorKind::Boss,
                                              &formats::WorldLocatorRecord::kind);
        if (marker == world.locators.end()) {
            return std::nullopt;
        }
        require(std::ranges::count(world.locators, LocatorKind::Boss,
                                   &formats::WorldLocatorRecord::kind) == 1,
                "multiple boss locators");
        require(finite(marker->position) && finite(marker->rotation), "nonfinite boss locator");
        require(marker->rotation.x == 0 && marker->rotation.z == 0,
                "tilted boss locator is not supported by the combat adapter");

        LegendsDreamcastBossEncounter encounter;
        encounter.map = "level" + level;
        encounter.nativeKind = nativeKind;
        encounter.nativeRealm = nativeRealm;
        encounter.archive = binding->archive;
        encounter.combat.kind = binding->kind;
        encounter.combat.position = marker->position;
        encounter.combat.yaw = marker->rotation.y;
        encounter.combat.counterpartStage = code;
        encounter.combat.counterpartWorld = binding->world;
        encounter.combat.soundBank = binding->sound;
        // Use the boss's actual body targets before the native arena's lower
        // destructible scenery. This changes aim priority, never collision.
        encounter.combat.rangedAimFirst = true;
        return encounter;
    }
    return std::nullopt;
}

} // namespace gdl::game
