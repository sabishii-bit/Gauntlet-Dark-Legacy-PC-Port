#include "engine/assets/WorldLayout.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "engine/assets/ParticleTemplateJson.h"
#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/core/Strings.h"
#include "engine/io/File.h"

namespace gdl {

namespace {

using Json = nlohmann::json;

constexpr std::array<std::string_view, static_cast<std::size_t>(LocatorKind::Count)> kKindNames{
    "none", "cameraStart", "cameraGame", "cameraAttractStart", "cameraAttract", "milestone",
    "boss", "start",       "sentry",     "triggerCamera",      "event"};

Vec3 readVec3(const Json& array) {
    if (!array.is_array() || array.size() != 3) {
        throw FormatError("world layout: expected three numbers");
    }
    return Vec3{array.at(0).get<float>(), array.at(1).get<float>(), array.at(2).get<float>()};
}

} // namespace

std::string_view locatorKindName(LocatorKind kind) {
    const auto index = static_cast<std::size_t>(kind);
    return index < kKindNames.size() ? kKindNames[index] : std::string_view{};
}

std::optional<LocatorKind> locatorKindFromName(std::string_view name) {
    for (std::size_t i = 0; i < kKindNames.size(); ++i) {
        if (kKindNames[i] == name) {
            return static_cast<LocatorKind>(i);
        }
    }
    return std::nullopt;
}

ItemInfo readItemInfo(const Json& entry) {
    ItemInfo info;
    info.type = entry.value("type", 0);
    info.subtype = entry.value("subtype", 0);
    info.name = normalizeAssetName(entry.value("name", std::string{}));
    info.radius = entry.value("radius", 0.0f);
    info.height = entry.value("height", 0.0f);
    info.xSize = entry.value("xSize", 0.0f);
    info.zSize = entry.value("zSize", 0.0f);
    info.collisionType = entry.value("collisionType", 0);
    info.choices = entry.value("choices", std::vector<std::int32_t>{});
    if (entry.contains("collisionOffset")) {
        info.collisionOffset = readVec3(entry.at("collisionOffset"));
    }
    info.objectFlags = entry.value("objectFlags", 0U);
    info.properties = entry.value("properties", 0U);
    info.value = entry.value("value", 0);
    info.armor = entry.value("armor", 0);
    info.hitPoints = entry.value("hitPoints", 0);
    info.activeType = entry.value("activeType", 0);
    info.activeOff = entry.value("activeOff", 0);
    info.activeOn = entry.value("activeOn", 0);
    return info;
}

ItemInstance readItemInstance(const Json& entry) {
    ItemInstance instance;
    instance.info = entry.value("info", -1);
    instance.minPlayers = entry.value("minPlayers", 0);
    instance.flags = entry.value("flags", 0U);
    instance.name = normalizeAssetName(entry.value("name", std::string{}));
    instance.position = readVec3(entry.at("position"));
    if (entry.contains("rotation")) {
        instance.rotation = readVec3(entry.at("rotation"));
    }
    const auto params = entry.value("params", std::vector<std::uint32_t>{});
    for (std::size_t i = 0; i < instance.params.size() && i < params.size(); ++i) {
        instance.params[i] = static_cast<std::uint8_t>(params[i]);
    }
    return instance;
}

bool WorldLayout::load(const std::filesystem::path& directory) {
    m_objects.clear();
    m_locators.clear();
    m_animations.clear();
    m_particles.clear();
    m_itemInfos.clear();
    m_itemInstances.clear();
    const std::filesystem::path file = directory / "world.json";
    try {
        const Json root = Json::parse(readTextFile(file), nullptr, true, true);
        if (root.contains("bounds")) {
            m_minBounds = readVec3(root.at("bounds").at("min"));
            m_maxBounds = readVec3(root.at("bounds").at("max"));
        }
        for (const Json& entry : root.at("objects")) {
            WorldObject object;
            object.name = entry.at("name").get<std::string>();
            object.position = readVec3(entry.at("position"));
            object.flags = entry.value("flags", 0U);
            object.objectFlags = entry.value("objectFlags", 0U);
            object.noCollision = entry.value("noCollision", false);
            object.next = entry.value("next", -1);
            object.child = entry.value("child", -1);
            object.radius = entry.value("radius", 0.0f);
            m_objects.push_back(std::move(object));
        }
        if (root.contains("animations") && root.at("animations").is_array()) {
            for (const Json& entry : root.at("animations")) {
                WorldAnimation animation;
                animation.object = entry.value("object", -1);
                animation.frames = entry.value("frames", 0);
                animation.state = entry.value("state", 0U);
                animation.start = entry.value("start", 0.0f);
                const Json& track = entry.at("track");
                animation.track.flags = static_cast<std::uint16_t>(track.value("flags", 0U));
                animation.track.frames = track.value("frames", std::vector<std::uint16_t>{});
                animation.track.values = track.value("values", std::vector<float>{});
                if (animation.object < 0 ||
                    static_cast<std::size_t>(animation.object) >= m_objects.size() ||
                    animation.track.frames.empty() ||
                    animation.track.values.size() !=
                        animation.track.frames.size() * animation.track.channelCount()) {
                    throw std::runtime_error("an animation names no object or has broken keys");
                }
                m_animations.push_back(std::move(animation));
            }
        }
        if (root.contains("particles") && root.at("particles").is_array()) {
            for (const Json& entry : root.at("particles")) {
                m_particles.push_back(readParticleTemplate(entry));
            }
        }
        if (root.contains("itemInfos") && root.at("itemInfos").is_array()) {
            for (const Json& entry : root.at("itemInfos")) {
                m_itemInfos.push_back(readItemInfo(entry));
            }
        }
        if (root.contains("itemInstances") && root.at("itemInstances").is_array()) {
            for (const Json& entry : root.at("itemInstances")) {
                m_itemInstances.push_back(readItemInstance(entry));
            }
        }
        if (root.contains("locators")) {
            for (const Json& entry : root.at("locators")) {
                WorldLocator locator;
                const auto kind = locatorKindFromName(entry.value("type", "none"));
                locator.kind = kind.value_or(LocatorKind::None);
                locator.delay = entry.value("delay", 0U);
                locator.next = entry.value("next", 0U);
                locator.position = readVec3(entry.at("position"));
                locator.rotation = readVec3(entry.at("rotation"));
                m_locators.push_back(locator);
            }
        }
    } catch (const std::exception& e) {
        log::warn("World layout {}: {}", file.string(), e.what());
        m_objects.clear();
        m_locators.clear();
        m_animations.clear();
        m_particles.clear();
        m_itemInfos.clear();
        m_itemInstances.clear();
        return false;
    }
    resolveParents();
    return !m_objects.empty();
}

const ParticleTemplate* WorldLayout::findParticleTemplate(char id) const {
    for (const ParticleTemplate& particle : m_particles) {
        if (particle.id == id) {
            return &particle;
        }
    }
    return nullptr;
}

/** Every object's children are the sibling list its `child` starts. */
void WorldLayout::resolveParents() {
    const auto count = static_cast<std::int32_t>(m_objects.size());
    for (std::int32_t i = 0; i < count; ++i) {
        std::int32_t child = m_objects[static_cast<std::size_t>(i)].child;
        for (std::int32_t guard = 0; child >= 0 && child < count && guard < count; ++guard) {
            WorldObject& object = m_objects[static_cast<std::size_t>(child)];
            if (object.parent >= 0) {
                break; // already claimed: a malformed list
            }
            object.parent = i;
            child = object.next;
        }
    }
}

const WorldLocator* WorldLayout::findLocator(LocatorKind kind, std::uint32_t next) const {
    for (const WorldLocator& locator : m_locators) {
        if (locator.kind == kind && locator.next == next) {
            return &locator;
        }
    }
    return nullptr;
}

Vec3 WorldLayout::worldPosition(std::size_t index) const {
    Vec3 position{0.0f, 0.0f, 0.0f};
    auto current = static_cast<std::int32_t>(index);
    for (std::size_t guard = 0;
         current >= 0 && static_cast<std::size_t>(current) < m_objects.size() &&
         guard < m_objects.size();
         ++guard) {
        const WorldObject& object = m_objects[static_cast<std::size_t>(current)];
        position += object.position;
        current = object.parent;
    }
    return position;
}

} // namespace gdl
