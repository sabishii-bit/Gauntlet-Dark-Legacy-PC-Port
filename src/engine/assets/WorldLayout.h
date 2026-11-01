#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/ObjectFlags.h"
#include "engine/assets/ParticleTemplate.h"
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
    bool noCollision = false; ///< its collision triangles are decoration only

    /** Level flags. */
    /** Set on most level objects; the original's draw never reads it. A mesh's own colour
     * stream decides its shading (WorldScene::shadeOf). */
    static constexpr u32 kPrelit = 0x2;
    static constexpr u32 kFloor = 0x4;        ///< floor surface with projectile impact clearance
    static constexpr u32 kParticles = 0x800;  ///< a particle system's marker, never drawn
    static constexpr u32 kAnimated = 0x1000;  ///< keyframed, or under something keyframed
    static constexpr u32 kReverse = 0x100000; ///< its animation plays backwards, once
    static constexpr u32 kOnce = 0x200000;    ///< its animation plays forwards, once
    /** Model layer flags, the shared object-layer word (ObjectFlags.h). */
    static constexpr u32 kNoDepthTest = object_flags::kNoDepthTest;
    static constexpr u32 kNoDepthWrite = object_flags::kNoDepthWrite;
    static constexpr u32 kSorted = object_flags::kSorted;
    static constexpr u32 kChrome = object_flags::kChrome;
    static constexpr u32 kSortBack = object_flags::kSortBack;
    static constexpr u32 kSortBehind = object_flags::kSortBehind;
    static constexpr u32 kAdditive = object_flags::kAdditive;

    bool particles() const { return (flags & kParticles) != 0; }
    bool sorted() const { return (objectFlags & kSorted) != 0; }
    bool additive() const { return (objectFlags & kAdditive) != 0; }
};

/** One placed object's keyframes: its frame count and the channels it moves. */
struct WorldAnimation {
    s32 object = -1;
    s32 frames = 0;
    u32 state = 0;
    f32 start = 0.0f;
    TrackInfo track;
};

/** One kind of item a level places. */
struct ItemInfo {
    static constexpr s32 kPowerup = 1;   ///< something to pick up
    static constexpr s32 kContainer = 2; ///< a chest or barrel
    static constexpr s32 kGenerator = 3;
    static constexpr s32 kPlacedEnemy = 4; ///< one enemy where the level puts it
    static constexpr s32 kTrigger = 5;     ///< a spot that drives a world object
    static constexpr s32 kGate = 7;        ///< a door a key opens
    static constexpr s32 kTrap = 8;
    static constexpr s32 kObstacle = 10; ///< scenery in the way: walls, rocks, falling pieces
    static constexpr s32 kTransporter = 11;
    static constexpr s32 kRotator = 12;    ///< turns a world object about its upright axis
    static constexpr s32 kChoiceList = -1; ///< not an item: a list to pick one from
    static constexpr s32 kGold = 1;        ///< the powerup subtype of gold
    static constexpr s32 kKey = 2;         ///< and of a key (a ring of them when stacked)
    static constexpr s32 kRunestone = 10;  ///< the powerup subtype of a runestone
    static constexpr s32 kScroll = 14;  ///< of a scroll: its instance's first parameter is its page
    static constexpr s32 kCrystal = 15; ///< of a realm's crystal
    static constexpr s32 kGargoyleKey = 16; ///< of a piece for the tower's gargoyles

    s32 type = 0;
    s32 subtype = 0;
    std::string name;
    f32 radius = 0.0f;
    f32 height = 0.0f;
    f32 xSize = 0.0f; ///< half its box across, for the box-shaped
    f32 zSize = 0.0f; ///< and along
    s32 collisionType = 0;
    u32 collisionFlags = 0; ///< kKeepsHeight
    Vec3 collisionOffset{0.0f, 0.0f, 0.0f};
    u32 objectFlags = 0;
    u32 properties = 0;
    s32 value = 0;
    s32 armor = 0;
    s32 hitPoints = 0;
    s32 activeType = 0; ///< kAlwaysActive, among what the kind does
    s32 activeOff = 0;
    s32 activeOn = 0;
    std::vector<s32> choices; ///< a choice list's item records

    static constexpr u32 kKeepsHeight =
        0x1; ///< collisionFlags: the authored height, never a floor's
    static constexpr u32 kAlwaysActive = 0x40; ///< activeType: a generator that breeds unseen
};

/** An item's authored collision surface, in the item's local coordinates. */
struct ItemCollisionTriangle {
    Vec3 normal{0, 1, 0};
    std::array<Vec3, 3> vertices{};
};

/** One item the level places: which kind, the party it takes to show it, its own name when
 * it has one, where it stands and the kind's parameters. */
struct ItemInstance {
    static constexpr u32 kAlwaysActive = 0x1; ///< flags: a generator that breeds unseen
    static constexpr u32 kNoGeometry = 0x2;   ///< flags: an empty node that keeps its trigger

    s32 info = -1;
    s32 minPlayers = 0;
    u32 flags = 0;
    std::string name;
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 rotation{0.0f, 0.0f, 0.0f}; ///< pitch, yaw, roll
    std::array<u8, 12> params{};
    std::vector<ItemCollisionTriangle> collision;
};

/** A marker point of a level: cameras, start positions and event spots. */
struct WorldLocator {
    LocatorKind kind = LocatorKind::None;
    u32 delay = 0;
    u32 next = 0; ///< which of its kind it is, e.g. the realm exit a camera belongs to
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 rotation{0.0f, 0.0f, 0.0f}; ///< pitch, yaw, roll in radians
};

/** One level's layout: where its objects stand and its marker points. */
class WorldLayout {
public:
    /** Reads WORLDS.PS2, or a legacy world.json if no native file exists. */
    bool load(const std::filesystem::path& directory);

    /** Builds validated common metadata from a native adapter, without an intermediate
     * manifest. Flags must already have common WorldObject semantics, not platform bits.
     * A locator-only layout is permitted; loaded() retains its mesh-roster meaning. */
    static WorldLayout fromDecoded(std::vector<WorldObject> objects,
                                   std::vector<WorldLocator> locators, Vec3 minBounds,
                                   Vec3 maxBounds);

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
    const WorldLocator* findLocator(LocatorKind kind, u32 next = 0) const;

    /** An object's position with every parent's offset applied. */
    Vec3 worldPosition(usize index) const;

private:
    bool loadNative(const std::filesystem::path& file);
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
