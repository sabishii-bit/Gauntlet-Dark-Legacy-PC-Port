#pragma once

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "engine/assets/AssetScope.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/WorldAnimator.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldScene.h"

#include "game/characters/Progression.h"
#include "game/characters/Relics.h"
#include "game/level/ItemFigure.h"

namespace gdl {
class NativeStateDigest;
}

namespace gdl::game {

/** A player stood in a trigger's spot without what it asks for. */
struct TriggerRefusal {
    s32 trigger = -1;
    s32 id = 0;           ///< the realm, or 100 plus the gargoyle tier
    bool crystals = true; ///< crystals were wanted, else the golden icons
    bool operator==(const TriggerRefusal&) const = default;
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
    s32 subtype = 0;
    bool closed = false;
    bool operator==(const TriggerOpening&) const = default;
};

/** A switch's rising activation edge, naming its camera marker and moving target. */
struct TriggerCameraCue {
    s32 id = 0;
    s32 target = -1;
    bool shakes = false;
    bool operator==(const TriggerCameraCue&) const = default;
};

/** An active participant's position, supporting floor and progression requirements. */
struct TriggerVisitor {
    Vec3 position{0.0f, 0.0f, 0.0f};
    f32 radius = 0.75f;
    std::array<s32, kRealmCount> crystals{};
    std::array<s32, Relics::kGargoyleKinds> gargoylePieces{};
    bool sumner = false; ///< Sumner passes every crystal gate
    s32 floorObject = -1;
    s32 party = -1;    ///< who it is, by place in the party
    f32 height = 5.0f; ///< full body height; collision tests use half of it
};

/** A player stepped alone onto a spot the whole party must stand on (items.c 3017-3049):
 * which of the party, and whether it is a platform (it must be stood on) or a trigger. */
struct TriggerLesson {
    s32 party = -1;
    bool platform = false;
    bool operator==(const TriggerLesson&) const = default;
};

/** How a trigger turns contact into its target's command. LevelTrigger::latchOf reads it from
 * the flag word; it is never stored. */
enum class TriggerLatch : u8 {
    Momentary, ///< opens its target while stood on: a pressure pad
    OpensOnce, ///< opens its target at a touch once it has settled; leaving does not shut it
    Closes,    ///< shuts its target at a touch once it has settled; leaving does not open it
    Toggles,   ///< turns its target the other way at a touch, then waits before the next
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
    /** The low byte of its flags (kKindMask): how it latches, how its target moves, whether it
     * wants a requirement met and whether it keeps its contacts. */
    u32 kind = 0;
    f32 radius = 0.0f;
    s32 sound = -1; ///< the slot of the sounds the target makes as it opens, or -1
    bool fired = false;
    bool shootable = false;      ///< a target on the wall, set off by what hits it
    f32 height = 0.0f;           ///< how tall it stands to what is thrown at it
    bool shot = false;           ///< hit since the last update
    bool occupied = false;       ///< someone was on it the last update
    bool movementLesson = false; ///< subtype 23's lesson, posted while its target exists
    bool forced = false;         ///< a special trigger remains activated without a visitor
    s32 minPlayers = 0;          ///< minimum joined party, or 10 plus an exact party size
    bool enabled = true;         ///< available to the current joined party
    u32 heldContacts = 0;        ///< previous effective contacts, retained during camera holds
    s32 floor = -1;              ///< animated supporting floor, when attached
    Mat4 localPlacement{1.0f};   ///< item placement in its supporting floor's space
    Mat4 placement{1.0f};        ///< current world placement, shared by contact and artwork
    bool operator==(const LevelTrigger&) const = default;

    /** Whether it wants every visitor to carry a realm's crystals first. */
    bool needsCrystals() const { return (flags & kRequirement) != 0 && id < kGargoyleIds; }
    /** Whether it guards a gargoyle gate, which wants the golden icons. */
    bool needsIcons() const { return (flags & kRequirement) != 0 && id >= kGargoyleIds; }
    /** The latch its flags name. */
    [[nodiscard]] TriggerLatch latch() const { return latchOf(flags); }

    static constexpr u32 kRequirement = 0x40;
    static constexpr u32 kOpensOnce = 0x2;
    static constexpr u32 kCloses = 0x1;
    static constexpr s32 kGargoyleIds = 100;  ///< ids from here guard the gargoyle gates
    static constexpr s32 kIconTierBase = 101; ///< a gargoyle gate's id less this is its tier
    static constexpr u32 kFades = 0x10;       ///< the target fades out rather than moving
    static constexpr u32 kToggles = 0x4;
    static constexpr u32 kOscillates = 0x20;
    static constexpr u32 kOnTarget = 0x100;
    static constexpr u32 kWholeParty = 0x400;
    static constexpr u32 kKeepContact = 0x80;
    static constexpr u32 kStaysSolid = 0x8;     ///< the target keeps blocking while it moves
    static constexpr u32 kWakesStatue = 0x2000; ///< going active wakes the nearest statue
    static constexpr u32 kDelaysWhenEmpty =
        0x800; ///< a toggle pad left empty waits before it toggles back
    static constexpr u32 kShakesCamera = 0x1000; ///< its camera cue shakes
    /** The low byte, the trigger type's own flags: the latch, the target's motion, the
     * requirement and keeping contact. */
    static constexpr u32 kKindMask = 0xFF;
    /** The kinds that hold their target; a pad with none of them is momentary. */
    static constexpr u32 kLatches = kCloses | kOpensOnce | kToggles;
    /** The kinds whose target keeps its trigger state from one pass to the next; every other
     * target's is wiped each pass, but a fading one's while something stands on it
     * (ProcessItemWobjs' gate, 0x80062F7C-0x80062F9C). */
    static constexpr u32 kKeepsState = kLatches | kRequirement;
    static constexpr s32 kShootableSubtype = 31;
    static constexpr s32 kMomentaryBridge = 20;   ///< subtype: fades out, back when left
    static constexpr s32 kLatchedBridge = 22;     ///< subtype: fades out once and stays
    static constexpr s32 kMovementLessonPad = 23; ///< subtype: its opening teaches the move
    /** Whether a subtype's target fades rather than moves: the bridges, which the level
     * sounds as such. */
    static constexpr bool bridgeSubtype(s32 subtype) {
        return subtype == kMomentaryBridge || subtype == kLatchedBridge;
    }
    /** The latch a flag word names: closing comes before opening once, and opening once before
     * toggling, as fireNode tests them; a word with none of them is momentary. */
    [[nodiscard]] static constexpr TriggerLatch latchOf(u32 flags) {
        if ((flags & kCloses) != 0) {
            return TriggerLatch::Closes;
        }
        if ((flags & kOpensOnce) != 0) {
            return TriggerLatch::OpensOnce;
        }
        if ((flags & kToggles) != 0) {
            return TriggerLatch::Toggles;
        }
        return TriggerLatch::Momentary;
    }
    /** Whether a kind's target is a momentary fade, a bridge revealed only while it is held:
     * it fades, and no latch, requirement or oscillation holds it. A rider on the revealed
     * bridge holds it too (ProcessItemWobjs). */
    [[nodiscard]] static constexpr bool momentaryFade(u32 kind) {
        return (kind & (kLatches | kRequirement | kOscillates)) == 0 && (kind & kFades) != 0;
    }
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
    LevelTriggers() = default;
    LevelTriggers(const LevelTriggers&) = delete;
    LevelTriggers& operator=(const LevelTriggers&) = delete;
    LevelTriggers(LevelTriggers&&) noexcept = default;
    LevelTriggers& operator=(LevelTriggers&&) noexcept = default;
    ~LevelTriggers() = default;

    struct Target {
        s32 subtype = 0;
        s32 object = -1;
        u32 kind = 0;
        bool animated = false;
        bool open = false;
        bool settled = true;
        bool pressed = false;
        bool wholePartyReady = false;
        bool returning = false;
        bool forced = false;
        /** A player stands on it (did_generate past one): a height target that goes clear of
         * collision as it moves holds still until they step off, as ProcessItemWobjs does. */
        bool ridden = false;
        f32 alpha = 1;
        Vec3 origin{0};
        f32 height = 0;
        f32 closedHeight = 0;
        f32 openHeight = 0;
        std::vector<usize> subtree;
        Vec3 spot{0};
        s32 sound = -1;
        bool operator==(const Target&) const = default;
        /** Whether it is a height target that waits for its riders to leave before moving:
         * neither animated nor a fade, and not one that keeps blocking while it moves. */
        bool holdsForRiders() const;
    };

    static constexpr std::array<s32, 9> kCrystalsToOpen{0, 15, 100, 125, 150, 175, 200, 225, 250};
    static constexpr f32 kFadeRate = 16.0f / 255.0f; ///< of full alpha, per game frame
    static constexpr f32 kRefusalCooldown = 2.5625f; ///< the original's, between two refusals
    static constexpr f32 kFrameRate = 30.0f;
    static constexpr f32 kReach = 3.0f; ///< legacy vertical reach, also used by rotator switches
    static constexpr f32 kMetReach =
        2.0f; ///< how much wider a crystal gate's spot is to a party that qualifies

    /** Takes the layout's trigger items, chains them, and holds their animated targets. */
    void bind(const WorldLayout& layout, WorldAnimator& animator, WorldCollision* collision);
    /** Visible pressure pads are separate from the world objects they activate. Their art is
     * looked up the way the original searches its headers: the level's archive, then the
     * realm's. */
    void bindFigures(RenderDevice& device, const WorldLayout& layout, const AssetScope& scope);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;
    void clear();
    usize size() const { return m_sim.triggers.size(); }
    const LevelTrigger& trigger(usize index) const { return m_sim.triggers[index]; }
    /** The targets the triggers drive, each once, in the order their first trigger named it. */
    std::span<const Target> targets() const { return m_sim.targets; }
    const ItemFigure* figure(usize index) const {
        return index < m_sim.figures.size() && m_sim.figures[index] ? &*m_sim.figures[index]
                                                                    : nullptr;
    }
    bool registerNativeResources(NativeStateResources& resources) const;
    /** Joined participants, including the fallen. Whole-party contact still counts only the
     * standing visitors. Without an explicit count, isolated callers use their visitors. */
    void setPlayerCount(s32 players);
    /** Preserve switch contacts while an event camera holds the party in place. */
    void setCameraHeld(bool held) { m_sim.cameraHeld = held; }
    /** A thrown weapon, a burst or a blast has hit a shootable trigger: on the next update it
     * goes off as though the whole party stood in it (ItemDamage's trigger case). */
    void shoot(usize index);
    /** The developer console presses any trigger: on the next update it goes off as though
     * the whole party stood in it, camera cue and all. False for one the party cannot use. */
    bool press(usize index);
    /** The crystals a realm's gate wants; none for realms without one. */
    static s32 crystalsNeeded(s32 realm);
    /** Whether one visitor meets a realm's crystal gate: Sumner, a completed (negative) count
     * or enough crystals. A gate opens for the party when anyone meets it. */
    static bool crystalsMet(const TriggerVisitor& visitor, s32 realm);
    /** Whether the target of a trigger has been opened. */
    bool opened(s32 object) const;
    bool settled(s32 object) const;
    /** Whether a trigger is moving this floor or the object it hangs from, as CritterBossAI
     * reads the trigger states of both (0x8003a340-0x8003a378): a target of a kind that keeps
     * its state, commanded and short of its endpoint. Retail's moving bit (0x10) also stands
     * on the pass that arrives; a target its riders hold still counts here. */
    bool movesFloor(s32 floorObject) const;
    /** How solid a fading target still looks, 1 shut to 0 gone. */
    f32 alphaOf(s32 object) const;

    /** Opens earned gates immediately. Ordinary pads react to contact on the first
     * update too, as ProcessItems does; spawning inside one must not disable it. */
    void openMet(std::span<const TriggerVisitor> visitors, WorldAnimator& animator,
                 WorldScene& scene, WorldCollision* collision);
    /** Places registered height targets before floor-bound items choose their parents. */
    void placeTargets(WorldScene& scene, WorldCollision* collision);
    /** Opens at once, as the level starts, the targets of the triggers with these ids (the
     * tower's lifts once the battlefield is entered, items.c 6956: the object is marked
     * activated and its animation put at its last frame); the triggers themselves are left
     * as they are. */
    void openAtOnce(std::span<const s32> ids, WorldAnimator& animator, WorldScene& scene,
                    WorldCollision* collision);
    /** Permanently activates an id and its chain, animated or already at the endpoint. */
    void activate(s32 id, WorldAnimator::Finish finish, WorldAnimator& animator, WorldScene& scene,
                  WorldCollision* collision);
    /** Fires the triggers visitors stand in and carries the fades on by `seconds`.
     * Other occupied floors retain revealed momentary bridges, but never press switches. */
    void update(f32 seconds, std::span<const TriggerVisitor> visitors, WorldAnimator& animator,
                WorldScene& scene, WorldCollision* collision,
                std::span<const s32> occupiedFloors = {});
    /** Target motion only. Switch contacts, sounds and other trigger events are not replayed. */
    struct Motion {
        static constexpr usize kMaxTargets = 2048;
        struct Target {
            s32 object = -1;
            bool open = false;
            bool settled = true;
            bool pressed = false;
            bool wholePartyReady = false;
            bool returning = false;
            bool forced = false;
            f32 alpha = 1;
            f32 height = 0;
            bool valid() const;
            bool operator==(const Target&) const = default;
        };
        f32 frameRemainder = 0;
        std::vector<Target> targets;
        bool valid() const;
        bool operator==(const Motion&) const = default;
    };
    Motion motion() const;
    bool acceptsMotion(const Motion& state) const;
    /** Restore target clocks atomically; geometry and collision are a separate checkpoint. */
    bool restoreMotion(const Motion& state);
    /** Continue the captured commands, without evaluating new contacts or emitting events. */
    bool advanceMotion(f32 seconds, const WorldAnimator& animator, WorldScene& scene,
                       WorldCollision* collision);
    /** The refusals and openings since the last call, and the targets that have just finished
     * opening before the party (faded to nothing or run to the end of their animation); each is
     * handed out once. */
    std::vector<TriggerRefusal> takeRefusals();
    std::vector<TriggerLesson> takeLessons() { return std::exchange(m_sim.lessons, {}); }
    std::vector<TriggerOpening> takeOpenings();
    std::vector<TriggerOpening> takeSettled();
    std::vector<TriggerCameraCue> takeCameraCues();
    /** The spots of the triggers flagged to wake a statue that went active since the last
     * call (fn_800606FC's 0x2000: the nearest placed enemy within ten is woken). */
    std::vector<Vec3> takeWakes() { return std::exchange(m_sim.wakes, {}); }
    bool hasWakes() const { return !m_sim.wakes.empty(); }

private:
    Target* targetOf(s32 object);
    const Target* targetOf(s32 object) const;
    static TriggerOpening openingOf(const Target& target, WorldAnimator::Finish finish);
    static bool fadesAway(const Target& target);
    static void applyAlpha(const Target& target, WorldScene& scene, WorldCollision* collision);
    static bool qualifies(const LevelTrigger& trigger, std::span<const TriggerVisitor> visitors);
    /** Whether anyone stands in the trigger's spot, `radius` wide. */
    bool reaches(const LevelTrigger& trigger, f32 radius, const TriggerVisitor& visitor) const;
    bool onTarget(const LevelTrigger& trigger, const TriggerVisitor& visitor) const;
    void refreshEligibility(s32 players);
    void collectContacts(usize index, u32 mask, std::span<u32> contacts) const;
    void fire(usize index, bool active, WorldAnimator::Finish finish, WorldAnimator& animator,
              WorldScene& scene, WorldCollision* collision);
    void fireNode(usize index, bool active, WorldAnimator::Finish finish, WorldAnimator& animator,
                  WorldScene& scene, WorldCollision* collision);
    /** True when the target's commanded state changes. */
    static bool openTarget(Target& target, bool open, WorldAnimator::Finish finish,
                           WorldAnimator& animator, WorldScene& scene, WorldCollision* collision);
    void stepMotion(f32 seconds, WorldScene& scene, WorldCollision* collision, bool report);
    void finishMotion(const WorldAnimator& animator, WorldCollision* collision, bool report);

    struct Simulation {
        std::vector<LevelTrigger> triggers;
        std::vector<Target> targets;
        std::vector<std::optional<ItemFigure>> figures;
        std::vector<TriggerRefusal> refusals;
        std::vector<TriggerLesson> lessons;
        std::vector<TriggerOpening> openings;
        std::vector<TriggerOpening> settled;
        std::vector<TriggerCameraCue> cameraCues;
        std::vector<Vec3> wakes;
        f32 frameRemainder = 0;
        f32 emptyToggleDelay = 0;
        s32 playerCount = -1;
        bool cameraHeld = false;
        bool operator==(const Simulation&) const = default;
    } m_sim;

public:
    /** Same-owner values for native rewind. Geometry, collision and animator state
     * belong to their own snapshots; restoring triggers never fires their commands. */
    class LocalSnapshot {
        friend class LevelTriggers;
        Simulation m_state;
        std::weak_ptr<const u8> m_binding;
        const LevelTriggers* m_owner = nullptr;
        u64 m_epoch = 0;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const;
    bool restoreLocal(const LocalSnapshot& saved);
    /** Canonical actual native values. Resource addresses and owner/restore
     * generations are excluded; the immutable hierarchy is included by value. */
    bool appendNativeDigest(NativeStateDigest& digest) const;
    /** Refuses foreign or stale snapshots before appending any bytes. */
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;

private:
    static void appendSimulationDigest(NativeStateDigest& digest, const Simulation& state);
    std::vector<s32> m_parents; ///< immutable hierarchy of the bound layout
    u64 m_localEpoch = 0;
    std::shared_ptr<const u8> m_checkpointBinding = std::make_shared<const u8>(0);
};

void visitNativeState(NativeStateDigest& digest, const TriggerRefusal& value);
void visitNativeState(NativeStateDigest& digest, const TriggerOpening& value);
void visitNativeState(NativeStateDigest& digest, const TriggerCameraCue& value);
void visitNativeState(NativeStateDigest& digest, const TriggerLesson& value);
void visitNativeState(NativeStateDigest& digest, const LevelTrigger& value);
void visitNativeState(NativeStateDigest& digest, const LevelTriggers::Target& value);

} // namespace gdl::game
