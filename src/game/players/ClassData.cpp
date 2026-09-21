#include "game/players/ClassData.h"

#include "engine/core/Strings.h"

#include <exception>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/io/File.h"

namespace gdl::game {

namespace {

constexpr std::array<std::string_view, kClassCount> kClassCodes{
    "WAR", "VAL", "WIZ", "ARC", "DWF", "KNI", "SOR", "JES", "MIN",
    "FAL", "JAC", "TIG", "OGR", "UNI", "MED", "HYE", "SUM"};
constexpr std::array<std::string_view, kColorCount> kColorCodes{"YEL", "BLU", "RED", "GRE"};
constexpr std::array<Color, kColorCount> kPlayerColors{
    Color::rgba(255, 255, 128), Color::rgba(135, 206, 235), Color::rgba(255, 192, 224),
    Color::rgba(128, 255, 128)};
constexpr std::array<Color, kColorCount> kBoxTints{Color::rgba(240, 240, 0),
                                                   Color::rgba(60, 60, 240), Color::rgba(240, 0, 0),
                                                   Color::rgba(0, 200, 0)};
constexpr std::array<Color, kColorCount> kIdleBoxTints{
    Color::rgba(180, 180, 60), Color::rgba(60, 60, 210), Color::rgba(200, 80, 80),
    Color::rgba(60, 150, 60)};

using Json = nlohmann::json;

void readRange(const Json& object, const char* key, f32& low, f32& high) {
    if (!object.contains(key) || !object.at(key).is_array() || object.at(key).size() != 2) {
        throw FormatError(std::string("class data: missing range ") + key);
    }
    low = object.at(key).at(0).get<f32>();
    high = object.at(key).at(1).get<f32>();
}

ClassStats parseClassStats(std::string_view text) {
    const Json root = Json::parse(text, nullptr, true, true);
    ClassStats stats;
    readRange(root, "fight", stats.fightMin, stats.fightMax);
    readRange(root, "speed", stats.speedMin, stats.speedMax);
    readRange(root, "armor", stats.armorMin, stats.armorMax);
    readRange(root, "magic", stats.magicMin, stats.magicMax);
    stats.height = root.value("height", 0.0f);
    stats.width = root.value("width", 0.0f);
    stats.collisionY = root.value("collisionY", 0.0f);
    stats.powerupTime = root.value("powerupTime", 1.0f);
    if (const auto offset = root.value("weaponOffset", std::vector<f32>{}); offset.size() == 3) {
        stats.weaponOffset = Vec3{offset[0], offset[1], offset[2]};
    }
    return stats;
}

} // namespace

std::string_view classCode(s32 classIndex) {
    if (classIndex < 0 || classIndex >= kClassCount) {
        return {};
    }
    return kClassCodes[static_cast<usize>(classIndex)];
}

std::string_view colorCode(s32 color) {
    if (color < 0 || color >= kColorCount) {
        return {};
    }
    return kColorCodes[static_cast<usize>(color)];
}

std::optional<s32> classIndexOf(std::string_view code) {
    const std::string wanted = normalizeAssetName(code);
    for (s32 i = 0; i < kClassCount; ++i) {
        if (kClassCodes[static_cast<usize>(i)] == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<s32> colorIndexOf(std::string_view code) {
    const std::string wanted = normalizeAssetName(code);
    for (s32 i = 0; i < kColorCount; ++i) {
        if (kColorCodes[static_cast<usize>(i)] == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

Color playerColor(s32 color) {
    if (color < 0 || color >= kColorCount) {
        return Color::white();
    }
    return kPlayerColors[static_cast<usize>(color)];
}

Color boxTint(s32 color, bool active) {
    if (color < 0 || color >= kColorCount) {
        return Color::white();
    }
    return active ? kBoxTints[static_cast<usize>(color)] : kIdleBoxTints[static_cast<usize>(color)];
}

bool classUnlocked(s32 classIndex, u16 unlockMask) {
    if (classIndex < 0 || classIndex >= kClassCount) {
        return false;
    }
    if (classIndex < kStartingClassCount) {
        return true;
    }
    return (unlockMask & (1U << (classIndex - kStartingClassCount))) != 0;
}

bool ClassDataSet::load(const std::filesystem::path& directory) {
    m_classes = {};
    m_loadedCount = 0;
    for (s32 i = 0; i < kClassCount; ++i) {
        const std::filesystem::path file = directory / (std::string(classCode(i)) + ".json");
        if (!std::filesystem::exists(file)) {
            continue;
        }
        try {
            m_classes[static_cast<usize>(i)] = parseClassStats(readTextFile(file));
            ++m_loadedCount;
        } catch (const std::exception& e) {
            log::warn("Class data: {}: {}", file.string(), e.what());
        }
    }
    if (m_loadedCount == 0) {
        log::warn("Class data: nothing readable under {}", directory.string());
    }
    return m_loadedCount > 0;
}

const ClassStats* ClassDataSet::stats(s32 classIndex) const {
    if (classIndex < 0 || classIndex >= kClassCount) {
        return nullptr;
    }
    const auto& entry = m_classes[static_cast<usize>(classIndex)];
    return entry.has_value() ? &*entry : nullptr;
}

} // namespace gdl::game
