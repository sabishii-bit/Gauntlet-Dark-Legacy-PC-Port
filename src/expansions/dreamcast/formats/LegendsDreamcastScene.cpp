#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <utility>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {
namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw FormatError(std::string("Dreamcast Legends scene: ") + message);
    }
}

bool finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

std::string lower(std::string_view text) {
    std::string result(text);
    for (char& c : result) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return result;
}

std::string levelCode(std::string_view name) {
    std::string result = lower(name);
    if (result.starts_with("level")) {
        result.erase(0, 5);
    }
    if (result.size() != 2 || result[0] < 'a' || result[0] > 'z' || result[1] < '1' ||
        result[1] > '9') {
        return {};
    }
    return result;
}

std::string text(std::span<const u8> bytes, usize at, usize length) {
    require(at <= bytes.size() && length <= bytes.size() - at, "text extent out of bounds");
    usize count = 0;
    while (count < length && bytes[at + count] != 0) {
        ++count;
    }
    return {std::bit_cast<const char*>(bytes.data() + at), count};
}

} // namespace

LegendsDreamcastScene LegendsDreamcastScene::parse(std::span<const u8> worlds) {
    require(worlds.size() >= 96, "truncated world header");
    require(readU32LE(worlds, 0) <= 16384 && readU32LE(worlds, 8) <= 2000000 &&
                readU32LE(worlds, 72) <= 65536 && readU32LE(worlds, 80) <= 65536 &&
                readU32LE(worlds, 88) <= 16384,
            "world table budget exceeded");
    const usize infoCount = readU32LE(worlds, 72);
    const usize infoOffset = readU32LE(worlds, 76);
    require(infoOffset <= worlds.size() &&
                infoCount <= (worlds.size() - infoOffset) / ItemInfoRecord::kSize,
            "item definitions out of bounds");
    for (usize i = 0; i < infoCount; ++i) {
        const usize at = infoOffset + i * ItemInfoRecord::kSize;
        if (readS32LE(worlds, at) == ItemInfoRecord::kChoiceList) {
            const s32 count = readS32LE(worlds, at + 4);
            require(count >= 0 && static_cast<usize>(count) <= ItemInfoRecord::kMostChoices,
                    "random item list exceeds record");
        }
    }

    // WorldLoad (1ST_READ.BIN 8c0e8b80) uses the same 80-byte definition and
    // 60-byte placement tables as this reader. Reuse the value records, not
    // GameCube fixture constructors or platform-specific collision semantics.
    WorldFile decoded = WorldFile::parse(worlds);
    for (const auto& info : decoded.itemInfos) {
        if (info.type == ItemInfoRecord::kChoiceList) {
            for (const s16 choice : info.choices) {
                require(choice >= 0 && static_cast<usize>(choice) < infoCount,
                        "random item definition reference out of bounds");
            }
            continue;
        }
        require(std::isfinite(info.radius) && std::isfinite(info.height) &&
                    std::isfinite(info.xSize) && std::isfinite(info.zSize) &&
                    finite(info.collisionOffset),
                "nonfinite item collision shape");
    }
    for (const auto& item : decoded.itemInstances) {
        require(item.info >= 0 && static_cast<usize>(item.info) < infoCount,
                "placed item definition reference out of bounds");
        require(item.minPlayers >= 0 && item.minPlayers <= 4, "invalid minimum player count");
        require(finite(item.position) && finite(item.rotation), "nonfinite item placement");
        require(item.triangleCount >= 0, "negative item collision count");
        if (item.triangleCount > 0) {
            require(item.triangleIndex >= 0 &&
                        static_cast<u32>(item.triangleIndex) <= decoded.collisionTriangleCount &&
                        static_cast<u32>(item.triangleCount) <=
                            decoded.collisionTriangleCount - static_cast<u32>(item.triangleIndex),
                    "item collision range out of bounds");
        }
    }
    LegendsDreamcastScene result;
    result.itemInfos = std::move(decoded.itemInfos);
    result.itemInstances = std::move(decoded.itemInstances);
    // SetLocators (8c0a78a0) admits types 1/2 and adds pi to their yaw,
    // but that is an eye-orbit angle, not WorldCamera's look direction.
    // 8c0774a0 adds radius * (cos(p)*sin(y), sin(p), cos(p)*cos(y))
    // to the attention (8c0ed040/8c0ec560). Our camera subtracts its
    // forward vector instead, so the two half-turns cancel. Keep the
    // authored yaw here; copying the native orbit yaw reverses the view.
    // Types 3/4 are attract cameras, not alternate follow markers.
    for (const auto& locator : decoded.locators) {
        if (locator.kind != LocatorKind::CameraStart && locator.kind != LocatorKind::CameraGame) {
            continue;
        }
        require(finite(locator.position) && finite(locator.rotation), "nonfinite camera marker");
        result.cameraMarkers.push_back(
            {locator.kind, locator.delay, locator.next, locator.position, locator.rotation});
    }
    return result;
}

std::string dreamcastLegendsWorldDataFile(std::string_view levelName) {
    const auto level = levelCode(levelName);
    if (level.empty()) {
        return {};
    }
    std::string_view realm;
    switch (level[0]) {
    case 'a': realm = "castle"; break;
    case 'b': realm = "mount"; break;
    case 'c': realm = "desert"; break;
    case 'd': realm = "forest"; break;
    case 'e': realm = "temple"; break;
    case 'f': realm = "hell"; break;
    case 'g': realm = "town"; break;
    case 'h': realm = "battle"; break;
    case 'i': realm = "ice"; break;
    case 'j': realm = "dream"; break;
    case 'k': realm = "sky"; break;
    case 's': realm = "secret"; break;
    case 't': realm = level == "t1" ? "tower" : "test"; break;
    default: return {};
    }
    return "worlddata/" + std::string(realm) + ".wad";
}

std::optional<LegendsDreamcastFixtureScope>
dreamcastLegendsFixtureScope(std::span<const u8> worlddata, std::string_view levelName) {
    const auto level = levelCode(levelName);
    if (level.empty()) {
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
    for (usize i = 0; i < count; ++i) {
        const usize at = directory + i * 12;
        const Section section{readU32LE(worlddata, at + 4), readU32LE(worlddata, at + 8)};
        require(section.offset >= 8 && section.offset <= directory && section.count <= 4096,
                "invalid worlddata section");
        require(sections.emplace(readU32LE(worlddata, at), section).second,
                "duplicate worlddata section");
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
        require(section.count <= (end - section.offset) / stride,
                "worlddata records exceed section");
        return section;
    };
    const auto realm = records(0x57524c44, 56);       // WRLD
    const auto levels = records(0x4c45564c, 144);     // LEVL
    const auto enemies = records(0x454e4d59, 24);     // ENMY
    const auto cameras = records(0x43414d53, 108);    // CAMS
    const auto bossCameras = records(0x4243414d, 84); // BCAM
    const auto audio = records(0x41554453, 60);       // AUDS
    const auto sounds = records(0x534e4453, 24);      // SNDS
    require(realm.count == 1, "worlddata requires one realm record");
    for (usize i = 0; i < levels.count; ++i) {
        const usize at = levels.offset + i * 144;
        if (lower(text(worlddata, at + 8, 4)) != level) {
            continue;
        }
        LegendsDreamcastFixtureScope result;
        result.title = text(worlddata, at + 20, 32);
        result.realmType = readS32LE(worlddata, realm.offset);
        result.generatorDifficulty = std::bit_cast<f32>(readU32LE(worlddata, at + 132));
        require(std::isfinite(result.generatorDifficulty) && result.generatorDifficulty > 0,
                "invalid generator difficulty baseline");
        // ResolveWorldDataPointers 8c0a17be..17dc binds LEVL's signed +0x7c
        // index to a distinct 84-byte BCAM record. BossCameraUpdate 8c082d20
        // takes precedence over the older CAMS transmitter camera.
        const s16 bossCameraIndex = std::bit_cast<s16>(readU16LE(worlddata, at + 124));
        if (bossCameras.count != 0 && bossCameraIndex >= 0) {
            require(static_cast<usize>(bossCameraIndex) < bossCameras.count,
                    "boss camera index out of bounds");
            const usize bossAt = bossCameras.offset + static_cast<usize>(bossCameraIndex) * 84;
            const auto scalar = [&](usize field) {
                const f32 value = std::bit_cast<f32>(readU32LE(worlddata, bossAt + field));
                require(std::isfinite(value), "nonfinite boss camera setting");
                return value;
            };
            const auto vector = [&](usize field) {
                return Vec3{scalar(field), scalar(field + 4), scalar(field + 8)};
            };
            BossCameraInfo camera;
            camera.flags = readU32LE(worlddata, bossAt);
            camera.maxYaw = scalar(4);
            camera.cosMaxYaw = scalar(8);
            camera.minDistance = scalar(12);
            camera.minPlayerDistance = scalar(16);
            camera.maxDistance = scalar(20);
            camera.maxPlayerDistance = scalar(24);
            camera.minPitch = scalar(28);
            camera.maxPitch = scalar(32);
            camera.minAttention = vector(36);
            camera.maxAttention = vector(48);
            camera.keyAttention = vector(60);
            camera.wizardAttention = vector(72);
            // BossCameraStart 8c0815a0 normalizes these two authored sentinels.
            if (camera.maxPlayerDistance == 0)
                camera.maxPlayerDistance = camera.maxDistance;
            if (camera.maxAttention.y >= 999998.0f)
                camera.maxAttention = camera.minAttention;
            require(camera.minDistance > 0 && camera.maxDistance >= camera.minDistance &&
                        camera.minPlayerDistance > 0 &&
                        camera.maxPlayerDistance >= camera.minPlayerDistance && camera.maxYaw >= 0,
                    "invalid boss camera range");
            result.bossCamera = camera;
        }
        const s16 audioIndex = std::bit_cast<s16>(readU16LE(worlddata, at + 74));
        if (audio.count != 0 && audioIndex >= 0) {
            require(static_cast<usize>(audioIndex) < audio.count, "audio index out of bounds");
            const usize audioAt = audio.offset + static_cast<usize>(audioIndex) * 60;
            const s16 entry = std::bit_cast<s16>(readU16LE(worlddata, audioAt + 16));
            if (entry >= 0) {
                require(static_cast<usize>(entry) < sounds.count,
                        "entry sound index out of bounds");
                result.entrySound =
                    text(worlddata, sounds.offset + static_cast<usize>(entry) * 24, 16);
            }
            // BindWorldData 8c0a1700 resolves S_%sNAME from the first enabled
            // level sharing this AUDS record. Secret/tower realms stay silent;
            // H4/H5 reuse H1/H2 names. AUDS.namesnd itself is -1 on disc.
            if (result.realmType != 12 && result.realmType != 13) {
                for (usize candidate = 0; candidate < levels.count; ++candidate) {
                    const usize row = levels.offset + candidate * 144;
                    if (readS32LE(worlddata, row + 4) == 0 ||
                        std::bit_cast<s16>(readU16LE(worlddata, row + 74)) != audioIndex)
                        continue;
                    std::string code = text(worlddata, row + 8, 4);
                    if (code == "H4")
                        code = "H1";
                    if (code == "H5")
                        code = "H2";
                    result.stageNameSound = "S_" + code + "NAME";
                    break;
                }
            }
            // AudioShootWall8c0e1220 -> AudioLevelSound8c0dc640 selects
            // AUDS+0x12 through the realm's sound-name table.
            const s16 hit = std::bit_cast<s16>(readU16LE(worlddata, audioAt + 18));
            if (hit >= 0) {
                require(static_cast<usize>(hit) < sounds.count, "wall sound index out of bounds");
                result.wallHitSound =
                    text(worlddata, sounds.offset + static_cast<usize>(hit) * 24, 16);
            }
        }
        const s16 cameraIndex = std::bit_cast<s16>(readU16LE(worlddata, at + 72));
        if (cameras.count != 0 && cameraIndex >= 0) {
            require(static_cast<usize>(cameraIndex) < cameras.count, "camera index out of bounds");
            const usize cameraAt = cameras.offset + static_cast<usize>(cameraIndex) * 108;
            const auto scalar = [&](usize field) {
                const f32 value = std::bit_cast<f32>(readU32LE(worlddata, cameraAt + field));
                require(std::isfinite(value), "nonfinite camera setting");
                return value;
            };
            LevelCameraInfo camera;
            camera.minPitch = scalar(8);
            camera.radiusMin = scalar(44);
            camera.radiusMax = scalar(48);
            require(camera.radiusMin > 0 && camera.radiusMax >= camera.radiusMin &&
                        camera.minPitch >= 0 && camera.minPitch <= kPi / 2,
                    "invalid camera range");
            // CAMS+0x24 controls limits. Unused exports can contain stale
            // bounds (including tower's copy of the mountain settings).
            camera.authoredBounds = worlddata[cameraAt + 36] != 0;
            if (camera.authoredBounds) {
                camera.boundsMin = {scalar(12), scalar(16), scalar(20)};
                camera.boundsMax = {scalar(24), scalar(28), scalar(32)};
            }
            result.camera = camera;
        }
        const std::string realmName = lower(text(worlddata, realm.offset + 4, 16));
        require(!realmName.empty() && std::ranges::all_of(realmName,
                                                          [](char c) {
                                                              return (c >= 'a' && c <= 'z') ||
                                                                     (c >= '0' && c <= '9') ||
                                                                     c == '_';
                                                          }),
                "invalid realm archive name");
        // 8c0a7300 loads WRLD.wave_name and optional LEVL.name item banks.
        // The selected six ENMY rows, not the whole realm's roster, populate
        // class->species in 8c0a2340. This preserves G4 and B4's different art.
        for (usize seat = 0; seat < 6; ++seat) {
            const s16 row = std::bit_cast<s16>(readU16LE(worlddata, at + 0x3c + seat * 2));
            require(row >= -1 && (row < 0 || static_cast<usize>(row) < enemies.count),
                    "level enemy roster reference out of bounds");
            if (row < 0) {
                continue;
            }
            const usize enemy = enemies.offset + static_cast<usize>(row) * 24;
            const s32 kind = readS32LE(worlddata, enemy);
            const s32 category = readS32LE(worlddata, enemy + 4);
            result.enemySounds.push_back(
                {kind, category, text(worlddata, enemy + 8, 8), text(worlddata, enemy + 16, 8)});
            if (category > 0 && static_cast<usize>(category) < result.enemyClasses.size()) {
                result.enemyClasses[static_cast<usize>(category)] = kind;
            }
        }
        // SetItem resolves model names from the already loaded world archive too.
        // Shoot-away walls and several doors are model entries there, not WOBJ
        // instances: omitting this lender loses the fixtures rather than avoiding
        // duplicate world geometry.
        result.archives = {"levels/level" + level,
                           "items/level" + level,
                           "items/" + realmName,
                           "generators",
                           "powerups",
                           "weapons"};
        return result;
    }
    return std::nullopt;
}

std::string dreamcastLegendsFixtureName(const ItemInfoRecord& info,
                                        const ItemInstanceRecord& instance,
                                        const LegendsDreamcastFixtureScope& scope) {
    // SetItem (8c0a8340) suppresses geometry when placement flag 2 is set.
    if ((instance.flags & 2U) != 0 || info.type == ItemInfoRecord::kChoiceList) {
        return {};
    }
    if (info.type == 10) {
        const s16 placedSubtype = std::bit_cast<s16>(readU16LE(instance.params, 0));
        const s32 subtype = placedSubtype > 0 ? placedSubtype : info.subtype;
        if (subtype == 41) {
            // SetItem's safe rock branch uses the definition name, not the
            // placement override, followed by the signed segment count (%s%d
            // at 8c12381c, consumed by 8c0a8340).
            const s16 count = std::bit_cast<s16>(readU16LE(instance.params, 2));
            return info.name + std::to_string(count);
        }
    }
    if (info.type != 3) {
        return instance.name.empty() ? info.name : instance.name;
    }
    // SetGenName (8c0a8100) replaces the normal instance-name override.
    const s32 tier = std::max<s32>(std::bit_cast<s8>(instance.params[0]), 1);
    if (scope.realmType == 5 || scope.realmType == 6) {
        return scope.realmType == 5 ? "GEN_SPECIAL2" : "GEN_SPECIAL3";
    }
    // Enemy prefix table at 8c121acc (36-byte entries), only generator species.
    constexpr std::array<std::string_view, 21> kPrefixes{
        "SCO", "TRO", "DEM", "RAT", "GRU", "KNI", "SNA", "SOR", "MUM", "SPI", "LIZ",
        "TRE", "MAG", "ZOM", "PLA", "WOL", "ICE", "WRM", "DOG", "SKE", "KNI"};
    const std::string named = lower(info.name);
    // MSVC's checked array iterator is not a pointer; keep the portable iterator type.
    // NOLINTNEXTLINE(readability-qualified-auto)
    const auto found = std::ranges::find_if(
        kPrefixes, [&](std::string_view prefix) { return lower(prefix) == named; });
    if (found == kPrefixes.end()) {
        return {};
    }
    s32 kind = static_cast<s32>(found - kPrefixes.begin());
    // 8c08ff40 receives initial generator c6=0: RAT uses the small class;
    // GRU/KNI prefer the primary class, then the secondary class, then itself.
    if (kind == 3) {
        kind = scope.enemyClasses[1];
    } else if (kind == 4 || kind == 5) {
        if (scope.enemyClasses[2] >= 0) {
            kind = scope.enemyClasses[2];
        } else if (scope.enemyClasses[3] >= 0) {
            kind = scope.enemyClasses[3];
        }
    }
    if (kind < 0 || static_cast<usize>(kind) >= kPrefixes.size()) {
        return {};
    }
    return "GEN_" + std::string(kPrefixes[static_cast<usize>(kind)]) + std::to_string(tier);
}

} // namespace gdl::formats
