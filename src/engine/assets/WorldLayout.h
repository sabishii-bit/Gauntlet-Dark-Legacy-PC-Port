#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

namespace gdl {

/** What a level's marker points stand for. */
enum class LocatorKind : u8 {
    None,
    CameraStart,
    CameraGame,
    CameraAttractStart,
    CameraAttract,
    Milestone,
    Boss,
    Start,
    Sentry,
    TriggerCamera,
    Event,
    Count
};

/** The manifest name of a locator kind ("cameraStart"); empty for an unknown value. */
std::string_view locatorKindName(LocatorKind kind);
std::optional<LocatorKind> locatorKindFromName(std::string_view name);

/** One placed object of a level: a mesh (or a group) at an offset from its parent. */
struct WorldObject {
    std::string name;
    Vec3 position{0.0f, 0.0f, 0.0f}; ///< relative to the parent
    u32 flags = 0;
    u32 objectFlags = 0; ///< the model layer's flags (chrome, blending, sorting)
    s32 next = -1;       ///< next sibling
    s32 child = -1;      ///< first child
    s32 parent = -1;     ///< resolved from the sibling lists
    f32 radius = 0.0f;

    static constexpr u32 kChrome = 0x8000;
};

/** A marker point of a level: cameras, start positions and event spots. */
struct WorldLocator {
    LocatorKind kind = LocatorKind::None;
    u32 delay = 0;
    u32 next = 0; ///< which of its kind it is, e.g. the realm exit a camera belongs to
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 rotation{0.0f, 0.0f, 0.0f}; ///< pitch, yaw, roll in radians
};

/** One unpacked level's layout: where its objects stand and its marker points. */
class WorldLayout {
public:
    /** Reads `directory/world.json`; false (with a warning) when missing or malformed. */
    bool load(const std::filesystem::path& directory);

    bool loaded() const { return !m_objects.empty(); }
    const std::vector<WorldObject>& objects() const { return m_objects; }
    const std::vector<WorldLocator>& locators() const { return m_locators; }
    const Vec3& minBounds() const { return m_minBounds; }
    const Vec3& maxBounds() const { return m_maxBounds; }

    /** The first locator of a kind with the given `next`, or nullptr. */
    const WorldLocator* findLocator(LocatorKind kind, u32 next = 0) const;

    /** An object's position with every parent's offset applied. */
    Vec3 worldPosition(usize index) const;

private:
    void resolveParents();

    std::vector<WorldObject> m_objects;
    std::vector<WorldLocator> m_locators;
    Vec3 m_minBounds{0.0f, 0.0f, 0.0f};
    Vec3 m_maxBounds{0.0f, 0.0f, 0.0f};
};

} // namespace gdl
