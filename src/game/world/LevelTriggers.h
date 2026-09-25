#pragma once

#include <array>
#include <memory>
#include <span>
#include <vector>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/WorldAnimator.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldScene.h"

#include "game/players/Progression.h"
#include "game/players/Relics.h"
#include "game/world/ItemFigure.h"

namespace gdl::game {

/** A player stood in a trigger's spot without what it asks for. */
struct TriggerRefusal {
    s32 trigger = -1;
    s32 id = 0;           ///< the realm, or 100 plus the gargoyle tier
    bool crystals = true; ///< crystals were wanted, else the golden icons
};

/** A target that opened this update (or, from takeSettled, has just finished opening): where
 * its trigger lies, whether it fades away, whether it opened at once (at the level's start)
 * rather than before the party, and the sound slot its trigger names (-1 for none). */
struct TriggerOpening {
    s32 target = -1;
    Vec3 spot{0.0f, 0.0f, 0.0f};
    bool fades = false;
    bool atOnce = false;
    s32 sound = -1;
};

/** A switch's rising activation edge, naming its camera marker and moving target. */
struct TriggerCameraCue {
    s32 id = 0;
    s32 target = -1;
};

/** An active participant's position, supporting floor and progression requirements. */
struct TriggerVisitor {
    Vec3 position{0.0f, 0.0f, 0.0f};
    f32 radius = 0.75f;
    std::array<s32, kRealmCount> crystals{};
    std::array<s32, Relics::kGargoyleKinds> gargoylePieces{};
    s32 floorObject = -1;
};

/** One of a level's triggers: a spot that, stepped into, opens the world object it names
 * and every trigger chained after it. */
struct LevelTrigger {
    s32 instance = -1; ///< the item instance it came from
    Vec3 spot{0.0f, 0.0f, 0.0f};
    s32 target = -1; ///< the world object it drives, or -1
    s32 id = 0;
    s32 nextId = 0;
    f32 refusalCooldown = 0.0f; ///< seconds before the trigger refuses anyone again
    f32 toggleCooldown = 0.0f;
    s32 next = -1;        ///< the trigger chained after this one, or -1
    bool chained = false; ///< another trigger's next: fired through it, never stepped on
    u32 flags = 0;        ///< the trigger's own flags
    u32 kind = 0;         ///< how the target moves: the flags the object's trigger type carries
    f32 radius = 0.0f;
    s32 sound = -1; ///< the slot of the sounds the target makes as it opens, or -1
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
    static constexpr u32 kToggles = 0x4;
    static constexpr u32 kOscillates = 0x20;
    static constexpr u32 kOnTarget = 0x100;
    static constexpr u32 kWholeParty = 0x400;
    static constexpr u32 kKeepContact = 0x80;
    static constexpr u32 kStaysSolid = 0x8; ///< the target keeps blocking while it moves
};

/**
 * A level's triggers as the original runs them: each trigger item names a world object
 * (which then waits at the first frame of its animation) and a spot; a player stepping in,
 * carrying the crystals the trigger asks for, opens the object and everything chained after
 * it. A trigger chained after another is set off only through it, never by a player. A
 * field flagged to fade thins out over half a second and stops blocking; an animated gate
 * plays forwards or backwards. Switch flags select latches, closing commands,
 * pressure plates or repeatable toggles, including whole-party lifts.
 */
class LevelTriggers {
public:
    static constexpr std::array<s32, 9> kCrystalsToOpen{0, 15, 100, 125, 150, 175, 200, 225, 250};
    static constexpr f32 kFadeRate = 16.0f / 255.0f;
    static constexpr f32 kRefusalCooldown =
        2.5625f; ///< the original's, between two refusals ///< of full alpha, per game frame
    static constexpr f32 kFrameRate = 30.0f;
    static constexpr f32 kReach = 3.0f; ///< how far above or below a trigger a visitor counts
    static constexpr f32 kMetReach =
        2.0f; ///< how much wider a crystal gate's spot is to a party that qualifies

    /** Takes the layout's trigger items, chains them, and holds their animated targets. */
    void bind(const WorldLayout& layout, WorldAnimator& animator, WorldCollision* collision);
    /** Visible pressure pads are separate from the world objects they activate. */
    void bindFigures(RenderDevice& device, const WorldLayout& layout, ItemArchive& items);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;
    void clear();
    usize size() const { return m_triggers.size(); }
    const LevelTrigger& trigger(usize index) const { return m_triggers[index]; }
    /** The crystals a realm's gate wants; none for realms without one. */
    static s32 crystalsNeeded(s32 realm);
    /** Whether the target of a trigger has been opened. */
    bool opened(s32 object) const;
    bool settled(s32 object) const;
    /** How solid a fading target still looks, 1 shut to 0 gone. */
    f32 alphaOf(s32 object) const;

    /** Opens earned gates immediately. Ordinary pads react to contact on the first
     * update too, as ProcessItems does; spawning inside one must not disable it. */
    void openMet(std::span<const TriggerVisitor> visitors, WorldAnimator& animator,
                 WorldScene& scene, WorldCollision* collision);
    /** Fires the triggers visitors stand in and carries the fades on by `seconds`. */
    void update(f32 seconds, std::span<const TriggerVisitor> visitors, WorldAnimator& animator,
                WorldScene& scene, WorldCollision* collision);
    /** The refusals and openings since the last call, and the targets that have just finished
     * opening before the party (faded to nothing or run to the end of their animation); each is
     * handed out once. */
    std::vector<TriggerRefusal> takeRefusals();
    std::vector<TriggerOpening> takeOpenings();
    std::vector<TriggerOpening> takeSettled();
    std::vector<TriggerCameraCue> takeCameraCues();

private:
    std::vector<std::unique_ptr<ItemFigure>> m_figures;
    struct Target {
        s32 object = -1;
        u32 kind = 0;
        bool animated = false;
        bool open = false;
        bool settled = true;
        bool pressed = false;   ///< contact combined across all switches naming this target
        bool returning = false; ///< descending half of an oscillating height target
        f32 alpha = 1.0f;
        Vec3 origin{0.0f};
        f32 height = 0.0f;
        f32 closedHeight = 0.0f;
        f32 openHeight = 0.0f;
        std::vector<usize> subtree;
        Vec3 spot{0.0f, 0.0f, 0.0f}; ///< the spot of the trigger that opened it
        s32 sound = -1;
    };

    Target* targetOf(s32 object);
    const Target* targetOf(s32 object) const;
    static TriggerOpening openingOf(const Target& target, bool atOnce);
    static bool fadesAway(const Target& target);
    static void applyAlpha(const Target& target, WorldScene& scene, WorldCollision* collision);
    static bool qualifies(const LevelTrigger& trigger, std::span<const TriggerVisitor> visitors);
    /** Whether anyone stands in the trigger's spot, `radius` wide. */
    bool visited(const LevelTrigger& trigger, f32 radius,
                 std::span<const TriggerVisitor> visitors) const;
    void fire(usize index, bool active, bool atOnce, WorldAnimator& animator, WorldScene& scene,
              WorldCollision* collision);
    /** True when the target's commanded state changes. */
    static bool openTarget(Target& target, bool open, bool atOnce, WorldAnimator& animator,
                           WorldScene& scene, WorldCollision* collision);

    std::vector<LevelTrigger> m_triggers;
    std::vector<Target> m_targets;
    std::vector<s32> m_parents;
    std::vector<TriggerRefusal> m_refusals;
    std::vector<TriggerOpening> m_openings;
    std::vector<TriggerOpening> m_settled;
    std::vector<TriggerCameraCue> m_cameraCues;
    f32 m_frameRemainder = 0.0f;
    f32 m_emptyToggleDelay = 0.0f;
};

} // namespace gdl::game
