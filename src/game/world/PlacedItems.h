#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/ParticleField.h"
#include "engine/world/TextureAnimator.h"
#include "engine/world/TreeModel.h"
#include "engine/world/TreePose.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

namespace gdl::game {

/** A character who can pick things up: where they stand and how big they are. */
struct Collector {
    Vec3 position{0.0f, 0.0f, 0.0f};
    float radius = 0.75f; ///< how far its touch reaches sideways
    float height = 1.0f;  ///< slack over an item's height up or down: half the toucher's height
};

/** Something a collector touched this update. */
struct Pickup {
    std::size_t item = 0;
    std::size_t collector = 0; ///< which of the collectors touched it
    std::int32_t subtype = 0;
    std::int32_t realm = -1; ///< for a crystal, the realm it counts towards
    std::int32_t amount = 0; ///< how much of it there is: gold, keys, health and so on
    std::uint32_t flags = 0; ///< its record's properties: a potion's kind, a powerup's which
    float strength = 0.0f;   ///< a powerup's
    Vec3 position{0.0f, 0.0f, 0.0f};
};

/** What the game makes of a touched item: nothing to leave it lying, else how much of its
 * amount is left there (none takes it away). */
using PickupJudge = std::function<std::optional<std::int32_t>(const Pickup&)>;

/**
 * The pickups a level places, as far as the tower needs them: each powerup's figure, found
 * by name in the archives it is given (the level's own before the powerups'), stood at its
 * instance a tenth of a unit above the floor the collision finds under it, and shown while
 * the party is large enough (an instance's minimum, or exactly that when it is marked so).
 * Each figure plays its first sequence over and over (the crystals turn). A collector
 * touching one takes it: it goes, and a crystal's gem burst plays where it was, its tree's
 * sequence carrying the emitters. The crystals can start unseen and be revealed the way
 * Sumner's welcome does it: from the world's origin outward, each fading in as the reveal
 * reaches it.
 */
class PlacedItems {
public:
    static constexpr float kFloorLift = 0.1f;
    static constexpr float kFloorReachAbove = 0.5f;       ///< a floor this far over the instance
    static constexpr float kFloorReachBelow = 3.0f;       ///< or this far under it
    static constexpr std::int32_t kExactPlayersMark = 10; ///< a minimum past this means exactly
    static constexpr float kFrameRate = 30.0f;
    static constexpr float kRevealSpread = 15.0f;        ///< units a second the reveal moves out
    static constexpr float kRevealLead = 1.75f;          ///< seconds' worth it starts out at
    static constexpr float kRevealStep = 8.0f / 255.0f;  ///< alpha gained a frame once reached
    static constexpr float kBurstFallbackSeconds = 1.0f; ///< a burst whose tree has no sequence
    /** The realm each crystal value counts towards, the way the original tables it. */
    static constexpr std::array<std::int32_t, 14> kCrystalRealms{4, 2,  6, 5, 1, 7,  8,
                                                                 3, 10, 5, 2, 5, 10, 15};
    /** The gem burst played for each realm's crystal, by realm. */
    static constexpr std::array<std::string_view, 9> kGemEffects{
        "",           "GETGEMORANGE", "GETGEMRED",    "GETGEMPURPLE",
        "GETGEMBLUE", "GETGEMGREEN",  "GETGEMYELLOW", "GETGEMWHITE",
        "GETGEMBLACK"};
    /** The bursts played for a runestone and a gargoyle piece taken. */
    static constexpr std::string_view kRuneEffect = "GETRUNE";
    static constexpr std::string_view kGargoyleEffect = "GETGARG";

    /** One placed pickup and its figure. */
    struct Item {
        std::string name;
        std::int32_t instance = -1; ///< which of the layout's instances it is
        std::int32_t info = -1;
        std::int32_t subtype = 0;
        std::int32_t value = 0; ///< its amount; a part taken leaves the rest
        std::uint32_t flags = 0;
        float strength = 0.0f;
        std::int32_t minPlayers = 0;
        float radius = 0.0f; ///< how far out it can be touched
        float height = 0.0f;
        Vec3 position{0.0f, 0.0f, 0.0f};
        Mat4 transform{1.0f};
        TreeModel model;
        TreePose pose;
        const TreeInfo* figure = nullptr; ///< the tree the model and pose come from
        ItemArchive* archive = nullptr;   ///< where the figure and its textures came from
        AnimationPlayer player;           ///< its first sequence, on a loop
        float alpha = 1.0f;               ///< under one while it fades in
        bool visible = false;
        bool taken = false;
        Vec3 velocity{0.0f, 0.0f, 0.0f}; ///< while thrown
        bool thrown = false;             ///< in the air or rolling, not yet at rest
        float noGrabSeconds = 0.0f;      ///< over nought, no one can take it yet

        /** Whether a party of `players` sees it. */
        bool shownTo(std::int32_t players) const;
        /** Whether a collector is on it. */
        bool touchedBy(const Collector& collector) const;
        /** Whether it may be taken now. */
        bool takeable() const { return visible && !taken && noGrabSeconds <= 0.0f; }
        /** The realm a crystal counts towards, or -1 for anything else. */
        std::int32_t realm() const;
    };

    /** A burst playing where an item was taken: its tree's sequence, the pose it drives, and
     * the field emitters riding the tree's particle nodes. */
    struct Effect {
        const TreeInfo* figure = nullptr;
        Vec3 position{0.0f, 0.0f, 0.0f};
        AnimationPlayer player;
        TreePose pose;
        std::vector<std::pair<std::size_t, std::size_t>> emitters; ///< tree node, field emitter
        float secondsLeft = 0.0f; ///< the time left when the tree has no sequence
        bool emitting = true;
    };

    /** Stands every powerup instance of the layout whose figure an archive holds; false
     * when none could be placed. The collision, when given, sets their height. */
    bool bind(RenderDevice& device, const WorldLayout& layout, const WorldCollision* collision,
              std::span<ItemArchive* const> archives);
    void clear();
    std::size_t size() const { return m_items.size(); }
    const Item& item(std::size_t index) const { return m_items[index]; }
    std::size_t visibleCount() const;
    std::int32_t playerCount() const { return m_players; }

    /** Shows the items a party of `players` sees. */
    void setPlayerCount(std::int32_t players);
    /** Takes whatever the collectors touch and starts its burst; the pickups are returned
     * for the game to hand out. With a `judge`, each touched item is its to take, take part
     * of or leave; only those it took from are returned. */
    std::vector<Pickup> collect(RenderDevice& device, std::span<const Collector> collectors,
                                const PickupJudge& judge = {});
    /** Drops a pickup of the level's item record named `name` at `position` (on the floor
     * under it, with a collision), as a chest or a fallen enemy leaves one; false when the
     * level has no such record or no archive its figure. */
    bool place(RenderDevice& device, std::string_view name, const Vec3& position,
               const WorldCollision* collision);
    /** Drops the item of record number `record`, holding `amount` when that is over none
     * (a chest's keys) rather than the record's own. */
    bool placeRecord(RenderDevice& device, std::int32_t record, const Vec3& position,
                     const WorldCollision* collision, std::int32_t amount = 0);
    /** Throws a pickup of the record named `name` from `position` at `velocity`: it sails
     * out, falls, bounces and rolls to a stop on the floor the collision finds (or is lost,
     * falling where there is none), and cannot be taken for `noGrabSeconds`. False as for
     * `place`. */
    bool throwItem(RenderDevice& device, std::string_view name, const Vec3& position,
                   const Vec3& velocity, const WorldCollision* collision, float noGrabSeconds);
    /** Whether any gold lies untaken. */
    bool goldLeft() const;
    /** Turns the figures, flies what was thrown and plays the bursts on by `seconds`. */
    void update(float seconds);
    // How a thrown item flies, as the original's coins do.
    static constexpr float kGravity = 32.0f; ///< units a second each second
    static constexpr float kBounce = 0.4f;   ///< of the fall's speed, back up
    static constexpr float kRestHeight =
        0.1f; ///< this near the floor it touches down; a bounce that would not clear it is the last
    static constexpr float kAirDrag = 0.5f;    ///< of its speed sideways lost a second aloft
    static constexpr float kGroundDrag = 4.0f; ///< and touching down
    static constexpr float kThrownFloorReach =
        100.0f; ///< how far under a flying item the floor is looked for
    std::size_t effectCount() const { return m_effects.size(); }
    const Effect& effect(std::size_t index) const { return m_effects[index]; }
    const ParticleField& bursts() const { return m_bursts; }
    /** Hides the crystals for reveal() to bring in. */
    void hideCrystals();
    /** Moves the reveal `seconds` on: crystals within its reach fade in. */
    void reveal(float seconds);
    bool revealing() const { return m_revealing; }
    /** Particles alive over every burst. */
    std::size_t burstParticleCount() const { return m_bursts.particleCount(); }
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const CameraFrame* camera = nullptr) const;

private:
    void startEffect(RenderDevice& device, std::string_view tree, const Vec3& position);

    std::vector<Item> m_items;
    std::vector<Effect> m_effects;
    ParticleField m_bursts;
    /** An archive's texture animations, shown on every item that came from it. */
    struct ArchiveMotion {
        ItemArchive* archive = nullptr;
        TextureAnimator animator;
    };

    void applyTextureMotion();

    std::vector<ItemArchive*> m_archives;
    std::vector<ItemInfo> m_infos; ///< the level's item records, for dropping more
    /** Builds the figure of an item named `name`; false when no archive holds it. */
    bool makeFigure(RenderDevice& device, Item& item);
    /** Flies a thrown item `seconds` on. */
    void fly(Item& item, float seconds);
    std::vector<ArchiveMotion> m_motions;
    const WorldCollision* m_collision = nullptr; ///< the floor thrown items land on
    std::int32_t m_players = 0;
    float m_frameRemainder = 0.0f;
    float m_revealTime = 0.0f;
    bool m_revealing = false;
};

} // namespace gdl::game
