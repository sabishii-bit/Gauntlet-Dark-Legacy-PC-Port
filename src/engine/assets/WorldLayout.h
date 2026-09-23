#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ParticleTemplate.h"
#include "engine/math/Math.h"

namespace gdl {

/** What a level's marker points stand for. */
enum class LocatorKind : std::uint8_t {
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
    std::uint32_t flags = 0;
    std::uint32_t objectFlags = 0; ///< the model layer's flags (chrome, blending, sorting)
    std::int32_t next = -1;        ///< next sibling
    std::int32_t child = -1;       ///< first child
    std::int32_t parent = -1;      ///< resolved from the sibling lists
    float radius = 0.0f;
    bool noCollision = false; ///< its collision triangles are decoration only

    /** Level flags. */
    static constexpr std::uint32_t kPrelit =
        0x2; ///< lit by its mesh's vertex colours, not the lights
    static constexpr std::uint32_t kParticles = 0x800;  ///< a particle system's marker, never drawn
    static constexpr std::uint32_t kAnimated = 0x1000;  ///< keyframed, or under something keyframed
    static constexpr std::uint32_t kReverse = 0x100000; ///< its animation plays backwards, once
    static constexpr std::uint32_t kOnce = 0x200000;    ///< its animation plays forwards, once
    /** Model layer flags. */
    static constexpr std::uint32_t kNoDepthWrite = 0x80;
    static constexpr std::uint32_t kSorted = 0x800; ///< drawn after the opaque, farthest first
    static constexpr std::uint32_t kChrome = 0x8000;
    static constexpr std::uint32_t kSortBack = 0x80000; ///< sorted as if farther, behind the rest
    static constexpr std::uint32_t kSortBehind = 0x400000; ///< sorted farther still
    static constexpr std::uint32_t kAdditive = 0x800000; ///< added onto the frame: glows and flames

    bool particles() const { return (flags & kParticles) != 0; }
    bool prelit() const { return (flags & kPrelit) != 0; }
    bool sorted() const { return (objectFlags & kSorted) != 0; }
    bool additive() const { return (objectFlags & kAdditive) != 0; }
};

/** One placed object's keyframes: its frame count and the channels it moves. */
struct WorldAnimation {
    std::int32_t object = -1;
    std::int32_t frames = 0;
    std::uint32_t state = 0;
    float start = 0.0f;
    TrackInfo track;
};

/** One kind of item a level places. */
struct ItemInfo {
    static constexpr std::int32_t kPowerup = 1;   ///< something to pick up
    static constexpr std::int32_t kContainer = 2; ///< a chest or barrel
    static constexpr std::int32_t kGenerator = 3;
    static constexpr std::int32_t kPlacedEnemy = 4; ///< one enemy where the level puts it
    static constexpr std::int32_t kTrigger = 5;     ///< a spot that drives a world object
    static constexpr std::int32_t kGate = 7;        ///< a door a key opens
    static constexpr std::int32_t kTrap = 8;
    static constexpr std::int32_t kChoiceList = -1; ///< not an item: a list to pick one from
    static constexpr std::int32_t kGold = 1;        ///< the powerup subtype of gold
    static constexpr std::int32_t kRunestone = 10;  ///< the powerup subtype of a runestone
    static constexpr std::int32_t kScroll =
        14; ///< of a scroll: its instance's first parameter is its page
    static constexpr std::int32_t kCrystal = 15;     ///< of a realm's crystal
    static constexpr std::int32_t kGargoyleKey = 16; ///< of a piece for the tower's gargoyles

    std::int32_t type = 0;
    std::int32_t subtype = 0;
    std::string name;
    float radius = 0.0f;
    float height = 0.0f;
    float xSize = 0.0f; ///< half its box across, for the box-shaped
    float zSize = 0.0f; ///< and along
    std::int32_t collisionType = 0;
    Vec3 collisionOffset{0.0f, 0.0f, 0.0f};
    std::uint32_t objectFlags = 0;
    std::uint32_t properties = 0;
    std::int32_t value = 0;
    std::int32_t armor = 0;
    std::int32_t hitPoints = 0;
    std::int32_t activeType = 0;
    std::int32_t activeOff = 0;
    std::int32_t activeOn = 0;
    std::vector<std::int32_t> choices; ///< a choice list's item records
};

/** One item the level places: which kind, the party it takes to show it, its own name when
 * it has one, where it stands and the kind's parameters. */
struct ItemInstance {
    std::int32_t info = -1;
    std::int32_t minPlayers = 0;
    std::uint32_t flags = 0;
    std::string name;
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 rotation{0.0f, 0.0f, 0.0f}; ///< pitch, yaw, roll
    std::array<std::uint8_t, 12> params{};
};

/** A marker point of a level: cameras, start positions and event spots. */
struct WorldLocator {
    LocatorKind kind = LocatorKind::None;
    std::uint32_t delay = 0;
    std::uint32_t next = 0; ///< which of its kind it is, e.g. the realm exit a camera belongs to
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
    const std::vector<WorldAnimation>& animations() const { return m_animations; }
    const std::vector<ParticleTemplate>& particleTemplates() const { return m_particles; }
    const std::vector<ItemInfo>& itemInfos() const { return m_itemInfos; }
    const std::vector<ItemInstance>& itemInstances() const { return m_itemInstances; }
    /** The template a marker's letter names, or null. */
    const ParticleTemplate* findParticleTemplate(char id) const;
    const Vec3& minBounds() const { return m_minBounds; }
    const Vec3& maxBounds() const { return m_maxBounds; }

    /** The first locator of a kind with the given `next`, or nullptr. */
    const WorldLocator* findLocator(LocatorKind kind, std::uint32_t next = 0) const;

    /** An object's position with every parent's offset applied. */
    Vec3 worldPosition(std::size_t index) const;

private:
    void resolveParents();

    std::vector<WorldObject> m_objects;
    std::vector<WorldLocator> m_locators;
    std::vector<WorldAnimation> m_animations;
    std::vector<ParticleTemplate> m_particles;
    std::vector<ItemInfo> m_itemInfos;
    std::vector<ItemInstance> m_itemInstances;
    Vec3 m_minBounds{0.0f, 0.0f, 0.0f};
    Vec3 m_maxBounds{0.0f, 0.0f, 0.0f};
};

} // namespace gdl
