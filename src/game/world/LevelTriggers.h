#pragma once

#include <array>
#include <span>
#include <vector>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/WorldAnimator.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldScene.h"

#include "game/players/Progression.h"

namespace gdl::game {

/** Someone who can set off a trigger: where they stand, how wide they are and the crystals
 * they carry towards each realm. */
struct TriggerVisitor {
    Vec3 position{0.0f, 0.0f, 0.0f};
    f32 radius = 0.75f;
    std::array<s32, kRealmCount> crystals{};
};

/** One of a level's triggers: a spot that, stepped into, opens the world object it names
 * and every trigger chained after it. */
struct LevelTrigger {
    s32 instance = -1; ///< the item instance it came from
    Vec3 spot{0.0f, 0.0f, 0.0f};
    s32 target = -1;   ///< the world object it drives, or -1
    s32 id = 0;
    s32 nextId = 0;
    s32 next = -1; ///< the trigger chained after this one, or -1
    u32 flags = 0; ///< the trigger's own flags
    u32 kind = 0;  ///< how the target moves: the flags the object's trigger type carries
    f32 radius = 0.0f;
    bool fired = false;

    /** Whether it wants every visitor to carry a realm's crystals first. */
    bool needsCrystals() const { return (flags & kRequirement) != 0 && id < kGargoyleIds; }
    /** Whether it guards a gargoyle gate, which wants the golden icons. */
    bool needsIcons() const { return (flags & kRequirement) != 0 && id >= kGargoyleIds; }

    static constexpr u32 kRequirement = 0x40;
    static constexpr u32 kOpensOnce = 0x2;
    static constexpr u32 kCloses = 0x1;
    static constexpr s32 kGargoyleIds = 100; ///< ids from here guard the gargoyle gates
    static constexpr u32 kFades = 0x10;      ///< the target fades out rather than moving
    static constexpr u32 kToggles = 0x20;
    static constexpr u32 kStaysSolid = 0x8;  ///< the target keeps blocking while it moves
};

/**
 * A level's triggers as the original runs them: each trigger item names a world object
 * (which then waits at the first frame of its animation) and a spot; a player stepping in,
 * carrying the crystals the trigger asks for, opens the object and everything chained after
 * it. A field flagged to fade thins out over half a second and stops blocking; an animated
 * gate plays its opening once. Gates asking for the golden icons stay shut for now.
 */
class LevelTriggers {
public:
    static constexpr std::array<s32, 9> kCrystalsToOpen{0, 15, 100, 125, 150, 175, 200, 225, 250};
    static constexpr f32 kFadeRate = 16.0f / 255.0f; ///< of full alpha, per game frame
    static constexpr f32 kFrameRate = 30.0f;
    static constexpr f32 kReach = 3.0f; ///< how far above or below a trigger a visitor counts

    /** Takes the layout's trigger items, chains them, and holds their animated targets. */
    void bind(const WorldLayout& layout, WorldAnimator& animator, WorldCollision* collision);
    void clear();
    usize size() const { return m_triggers.size(); }
    const LevelTrigger& trigger(usize index) const { return m_triggers[index]; }
    /** The crystals a realm's gate wants; none for realms without one. */
    static s32 crystalsNeeded(s32 realm);
    /** Whether the target of a trigger has been opened. */
    bool opened(s32 object) const;
    /** How solid a fading target still looks, 1 shut to 0 gone. */
    f32 alphaOf(s32 object) const;

    /** Opens at once whatever the party already qualifies for, as a level does when it
     * starts. */
    void openMet(std::span<const TriggerVisitor> visitors, WorldAnimator& animator,
                 WorldScene& scene, WorldCollision* collision);
    /** Fires the triggers visitors stand in and carries the fades on by `seconds`. */
    void update(f32 seconds, std::span<const TriggerVisitor> visitors, WorldAnimator& animator,
                WorldScene& scene, WorldCollision* collision);

private:
    struct Target {
        s32 object = -1;
        u32 kind = 0;
        bool animated = false;
        bool open = false;
        f32 alpha = 1.0f;
    };

    Target* targetOf(s32 object);
    const Target* targetOf(s32 object) const;
    static bool qualifies(const LevelTrigger& trigger, std::span<const TriggerVisitor> visitors);
    void fire(usize index, bool atOnce, WorldAnimator& animator, WorldScene& scene,
              WorldCollision* collision);
    static void openTarget(Target& target, bool atOnce, WorldAnimator& animator,
                           WorldScene& scene, WorldCollision* collision);

    std::vector<LevelTrigger> m_triggers;
    std::vector<Target> m_targets;
    f32 m_frameRemainder = 0.0f;
};

} // namespace gdl::game
