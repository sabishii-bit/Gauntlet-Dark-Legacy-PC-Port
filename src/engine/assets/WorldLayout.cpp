#include "engine/assets/WorldLayout.h"

#include <array>
#include <exception>

#include <nlohmann/json.hpp>

#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/io/File.h"

namespace gdl {

namespace {

using Json = nlohmann::json;

constexpr std::array<std::string_view, static_cast<usize>(LocatorKind::Count)> kKindNames{
    "none",      "cameraStart", "cameraGame", "cameraAttractStart", "cameraAttract", "milestone",
    "boss",      "start",       "sentry",     "triggerCamera",      "event"};

Vec3 readVec3(const Json& array) {
    if (!array.is_array() || array.size() != 3) {
        throw FormatError("world layout: expected three numbers");
    }
    return Vec3{array.at(0).get<f32>(), array.at(1).get<f32>(), array.at(2).get<f32>()};
}

} // namespace

std::string_view locatorKindName(LocatorKind kind) {
    const auto index = static_cast<usize>(kind);
    return index < kKindNames.size() ? kKindNames[index] : std::string_view{};
}

std::optional<LocatorKind> locatorKindFromName(std::string_view name) {
    for (usize i = 0; i < kKindNames.size(); ++i) {
        if (kKindNames[i] == name) {
            return static_cast<LocatorKind>(i);
        }
    }
    return std::nullopt;
}

bool WorldLayout::load(const std::filesystem::path& directory) {
    m_objects.clear();
    m_locators.clear();
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
            object.next = entry.value("next", -1);
            object.child = entry.value("child", -1);
            object.radius = entry.value("radius", 0.0f);
            m_objects.push_back(std::move(object));
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
        return false;
    }
    resolveParents();
    return !m_objects.empty();
}

/** Every object's children are the sibling list its `child` starts. */
void WorldLayout::resolveParents() {
    const auto count = static_cast<s32>(m_objects.size());
    for (s32 i = 0; i < count; ++i) {
        s32 child = m_objects[static_cast<usize>(i)].child;
        for (s32 guard = 0; child >= 0 && child < count && guard < count; ++guard) {
            WorldObject& object = m_objects[static_cast<usize>(child)];
            if (object.parent >= 0) {
                break; // already claimed: a malformed list
            }
            object.parent = i;
            child = object.next;
        }
    }
}

const WorldLocator* WorldLayout::findLocator(LocatorKind kind, u32 next) const {
    for (const WorldLocator& locator : m_locators) {
        if (locator.kind == kind && locator.next == next) {
            return &locator;
        }
    }
    return nullptr;
}

Vec3 WorldLayout::worldPosition(usize index) const {
    Vec3 position{0.0f, 0.0f, 0.0f};
    auto current = static_cast<s32>(index);
    for (usize guard = 0; current >= 0 && static_cast<usize>(current) < m_objects.size() &&
                          guard < m_objects.size();
         ++guard) {
        const WorldObject& object = m_objects[static_cast<usize>(current)];
        position += object.position;
        current = object.parent;
    }
    return position;
}

} // namespace gdl
