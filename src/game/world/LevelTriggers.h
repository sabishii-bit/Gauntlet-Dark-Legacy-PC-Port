#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include "engine/assets/WorldLayout.h"
#include "engine/math/Math.h"
#include "engine/world/WorldAnimator.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldScene.h"

#include "game/players/Progression.h"

namespace gdl::game {

/** Someone who can set off a trigger: where they stand, how wide they are and the crystals
 * they carry towards each realm. */
/** A player stood in a trigger's spot without what it asks for. */
struct TriggerRefusal {
    int trigger = -1;
    int id = 0;           ///< the realm, or 100 plus the gargoyle tier
    bool crystals = true; ///< crystals were wanted, else the golden icons
};

/** A target that opened this update (or, from takeSettled, has just finished opening): where
 * its trigger lies, whether it fades away, whether it opened at once (at the level's start)
 * rather than before the party, and the sound slot its trigger names (-1 for none). */
struct TriggerOpening {
    int target = -1;
    Vec3 spot{0.0f, 0.0f, 0.0f};
    bool fades = false;
    bool atOnce = false;
    int sound = -1;
};

struct TriggerVisitor {
    Vec3 position{0.0f, 0.0f, 0.0f};
    float radius = 0.75f;
    std::array<int, kRealmCount> crystals{};
};

/** One of a level's triggers: a spot that, stepped into, opens the world object it names
 * and every trigger chained after it. */
struct LevelTrigger {
    int instance = -1; ///< the item instance it came from
    Vec3 spot{0.0f, 0.0f, 0.0f};
    int target = -1; ///< the world object it drives, or -1
    int id = 0;
    int nextId = 0;
    float refusalCooldown = 0.0f; ///< seconds before the trigger refuses anyone again
    int next = -1;                ///< the trigger chained after this one, or -1
    bool chained = false;         ///< another trigger's next: fired through it, never stepped on
    unsigned int flags = 0;       ///< the trigger's own flags
    unsigned int kind = 0; ///< how the target moves: the flags the object's trigger type carries
    float radius = 0.0f;
    int sound = -1; ///< the slot of the sounds the target makes as it opens, or -1
    bool fired = false;
    bool occupied = false; ///< the party stood in it as the level opened: it waits for them
                           ///< to leave and come back before it goes off (it still refuses)

    /** Whether it wants every visitor to carry a realm's crystals first. */
    bool needsCrystals() const { return (flags & kRequirement) != 0 && id < kGargoyleIds; }
    /** Whether it guards a gargoyle gate, which wants the golden icons. */
    bool needsIcons() const { return (flags & kRequirement) != 0 && id >= kGargoyleIds; }

    static constexpr unsigned int kRequirement = 0x40;
    static constexpr unsigned int kOpensOnce = 0x2;
    static constexpr unsigned int kCloses = 0x1;
    static constexpr int kGargoyleIds = 100;     ///< ids from here guard the gargoyle gates
    static constexpr unsigned int kFades = 0x10; ///< the target fades out rather than moving
    static constexpr unsigned int kToggles = 0x20;
    static constexpr unsigned int kStaysSolid = 0x8; ///< the target keeps blocking while it moves
};

/**
 * A level's triggers as the original runs them: each trigger item names a world object
 * (which then waits at the first frame of its animation) and a spot; a player stepping in,
 * carrying the crystals the trigger asks for, opens the object and everything chained after
 * it. A trigger chained after another is set off only through it, never by a player. A
 * field flagged to fade thins out over half a second and stops blocking; an animated gate
 * plays its opening once. Gates asking for the golden icons stay shut for now.
 */
class LevelTriggers {
public:
    static constexpr std::array<int, 9> kCrystalsToOpen{0, 15, 100, 125, 150, 175, 200, 225, 250};
    static constexpr float kFadeRate = 16.0f / 255.0f;
    static constexpr float kRefusalCooldown =
        2.5625f; ///< the original's, between two refusals ///< of full alpha, per game frame
    static constexpr float kFrameRate = 30.0f;
    static constexpr float kReach = 3.0f; ///< how far above or below a trigger a visitor counts
    static constexpr float kMetReach =
        2.0f; ///< how much wider a crystal gate's spot is to a party that qualifies

    /** Takes the layout's trigger items, chains them, and holds their animated targets. */
    void bind(const WorldLayout& layout, WorldAnimator& animator, WorldCollision* collision);
    void clear();
    std::size_t size() const { return m_triggers.size(); }
    const LevelTrigger& trigger(std::size_t index) const { return m_triggers[index]; }
    /** The crystals a realm's gate wants; none for realms without one. */
    static int crystalsNeeded(int realm);
    /** Whether the target of a trigger has been opened. */
    bool opened(int object) const;
    /** How solid a fading target still looks, 1 shut to 0 gone. */
    float alphaOf(int object) const;

    /** Opens at once whatever the party already qualifies for, as a level does when it
     * starts; a spot the party is already standing in (a scenario's doing: nobody starts
     * on one) waits for them to leave it and come back. */
    void openMet(std::span<const TriggerVisitor> visitors, WorldAnimator& animator,
                 WorldScene& scene, WorldCollision* collision);
    /** Fires the triggers visitors stand in and carries the fades on by `seconds`. */
    void update(float seconds, std::span<const TriggerVisitor> visitors, WorldAnimator& animator,
                WorldScene& scene, WorldCollision* collision);
    /** The refusals and openings since the last call, and the targets that have just finished
     * opening before the party (faded to nothing or run to the end of their animation); each is
     * handed out once. */
    std::vector<TriggerRefusal> takeRefusals();
    std::vector<TriggerOpening> takeOpenings();
    std::vector<TriggerOpening> takeSettled();

private:
    struct Target {
        int object = -1;
        unsigned int kind = 0;
        bool animated = false;
        bool open = false;
        bool settled = false; ///< done opening, or opened at once
        float alpha = 1.0f;
        Vec3 spot{0.0f, 0.0f, 0.0f}; ///< the spot of the trigger that opened it
        int sound = -1;
    };

    Target* targetOf(int object);
    const Target* targetOf(int object) const;
    static TriggerOpening openingOf(const Target& target, bool atOnce);
    static bool qualifies(const LevelTrigger& trigger, std::span<const TriggerVisitor> visitors);
    /** Whether anyone stands in the trigger's spot, `radius` wide. */
    static bool visited(const LevelTrigger& trigger, float radius,
                        std::span<const TriggerVisitor> visitors);
    void fire(std::size_t index, bool atOnce, WorldAnimator& animator, WorldScene& scene,
              WorldCollision* collision);
    /** True when the target opened now, not earlier. */
    static bool openTarget(Target& target, bool atOnce, WorldAnimator& animator, WorldScene& scene,
                           WorldCollision* collision);

    std::vector<LevelTrigger> m_triggers;
    std::vector<Target> m_targets;
    std::vector<TriggerRefusal> m_refusals;
    std::vector<TriggerOpening> m_openings;
    std::vector<TriggerOpening> m_settled;
    float m_frameRemainder = 0.0f;
};

} // namespace gdl::game
