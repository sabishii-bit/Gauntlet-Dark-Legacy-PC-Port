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
    const auto vec3 = [](const Json& object, const char* key) {
        const auto values = object.value(key, std::vector<f32>{});
        return values.size() == 3 ? Vec3{values[0], values[1], values[2]} : Vec3{0.0f};
    };
    if (const auto moves = root.find("moves"); moves != root.end() && moves->is_object()) {
        stats.moves.turboAThrow = moves->value("turboAThrow", -1);
        stats.moves.turboB = moves->value("turboB", -1);
        stats.moves.turboC1 = moves->value("turboC1", -1);
        stats.moves.turboC2 = moves->value("turboC2", -1);
        stats.moves.combo1 = moves->value("combo1", -1);
        stats.moves.comboHit = moves->value("comboHit", -1);
    }
    for (const Json& entry : root.value("moveEffects", Json::array())) {
        MoveEffect effect;
        effect.next = entry.value("next", -1);
        effect.tree = entry.value("tree", std::string{});
        effect.sound = entry.value("sound", std::string{});
        effect.offset = vec3(entry, "offset");
        effect.scale = entry.value("scale", 1.0f);
        stats.moveEffects.push_back(std::move(effect));
    }
    for (const Json& entry : root.value("moveStrikes", Json::array())) {
        MoveStrike strike;
        strike.type = entry.value("type", MoveStrike::kBursts);
        strike.hitRadius = entry.value("hitRadius", 0.0f);
        strike.radius = entry.value("radius", 0.0f);
        strike.delay = entry.value("delay", 0.0f);
        strike.maxTime = entry.value("maxTime", 0.0f);
        strike.arc = entry.value("arc", -1.0f);
        strike.offset = vec3(entry, "offset");
        strike.amount = entry.value("amount", 0.0f);
        // The original flies it half way between its least speed and its most.
        const f32 least = entry.value("speedMin", 0.0f);
        strike.speed = least + 0.5f * (entry.value("speedMax", least) - least);
        strike.angle = entry.value("angle", 0.0f);
        strike.damageType = entry.value("damageType", 0U);
        strike.hitEffect = entry.value("hitEffect", -1);
        strike.effect = entry.value("effect", -1);
        strike.loopEffect = entry.value("loopEffect", -1);
        strike.endFrame = entry.value("endFrame", -1);
        strike.flags = entry.value("flags", 0);
        strike.help = entry.value("help", -1);
        strike.next = entry.value("next", -1);
        strike.startFrame = entry.value("startFrame", 0);
        stats.moveStrikes.push_back(strike);
    }
    return stats;
}

} // namespace

f32 MoveStrike::dimming() const {
    constexpr s32 kCombo = 0x2000;
    constexpr s32 kGreater = 0x20;
    constexpr s32 kLesser = 0x10;
    if ((flags & kCombo) != 0) {
        return -0.8f;
    }
    if ((flags & kGreater) != 0) {
        return -0.6f;
    }
    return (flags & kLesser) != 0 ? -0.4f : 0.0f;
}

std::vector<s32> ClassStats::strikesOf(s32 first) const {
    std::vector<s32> chain;
    // A chain is followed once round at most.
    for (s32 at = first; at >= 0 && static_cast<usize>(at) < moveStrikes.size() &&
                         chain.size() < moveStrikes.size();
         at = moveStrikes[static_cast<usize>(at)].next) {
        chain.push_back(at);
    }
    return chain;
}

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
