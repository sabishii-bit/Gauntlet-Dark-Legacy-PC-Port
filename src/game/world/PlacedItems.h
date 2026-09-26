#pragma once

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
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
    f32 radius = 0.75f; ///< how far its touch reaches sideways
    f32 height = 1.0f;  ///< slack over an item's height up or down: half the toucher's height
};

/** Something a collector touched this update. */
struct Pickup {
    usize item = 0;
    usize collector = 0; ///< which of the collectors touched it
    s32 subtype = 0;
    s32 realm = -1;      ///< for a crystal, the realm it counts towards
    s32 amount = 0;      ///< how much of it there is: gold, keys, health and so on
    u32 flags = 0;       ///< its record's properties: a potion's kind, a powerup's which
    f32 strength = 0.0f; ///< a powerup's
    Vec3 position{0.0f, 0.0f, 0.0f};
};

/** What the game makes of a touched item: nothing to leave it lying, else how much of its
 * amount is left there (none takes it away). */
using PickupJudge = std::function<std::optional<s32>(const Pickup&)>;

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
    static constexpr f32 kFloorLift = 0.1f;
    static constexpr f32 kThrownFloorLift = 1.0f; ///< ProcessSpewItems rests coins above the floor
    static constexpr f32 kFloorReachAbove = 0.5f; ///< a floor this far over the instance
    static constexpr f32 kFloorReachBelow = 3.0f; ///< or this far under it
    static constexpr s32 kExactPlayersMark = 10;  ///< a minimum past this means exactly
    static constexpr f32 kFrameRate = 30.0f;
    static constexpr f32 kRevealSpread = 15.0f;        ///< units a second the reveal moves out
    static constexpr f32 kRevealLead = 1.75f;          ///< seconds' worth it starts out at
    static constexpr f32 kRevealStep = 8.0f / 255.0f;  ///< alpha gained a frame once reached
    static constexpr f32 kBurstFallbackSeconds = 1.0f; ///< a burst whose tree has no sequence
    /** The realm each crystal value counts towards, the way the original tables it. */
    static constexpr std::array<s32, 14> kCrystalRealms{4, 2,  6, 5, 1, 7,  8,
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
        s32 instance = -1; ///< which of the layout's instances it is
        s32 info = -1;
        s32 subtype = 0;
        s32 value = 0; ///< its amount; a part taken leaves the rest
        u32 flags = 0;
        f32 strength = 0.0f;
        s32 minPlayers = 0;
        f32 radius = 0.0f; ///< how far out it can be touched
        f32 height = 0.0f;
        Vec3 position{0.0f, 0.0f, 0.0f};
        Mat4 transform{1.0f};
        TreeModel model;
        TreePose pose;
        const TreeInfo* figure = nullptr; ///< the tree the model and pose come from
        ItemArchive* archive = nullptr;   ///< where the figure and its textures came from
        AnimationPlayer player;           ///< its first sequence, on a loop
        f32 alpha = 1.0f;                 ///< under one while it fades in
        bool visible = false;
        bool taken = false;
        bool contained = false;          ///< opening chest owns it; not yet collectible
        Vec3 velocity{0.0f, 0.0f, 0.0f}; ///< while thrown
        bool thrown = false;             ///< in the air or rolling, not yet at rest
        f32 noGrabSeconds = 0.0f;        ///< over nought, no one can take it yet

        /** Whether a party of `players` sees it. */
        bool shownTo(s32 players) const;
        /** Whether a collector is on it. */
        bool touchedBy(const Collector& collector) const;
        /** Whether it may be taken now. */
        bool takeable() const { return visible && !taken && !contained && noGrabSeconds <= 0.0f; }
        /** The realm a crystal counts towards, or -1 for anything else. */
        s32 realm() const;
    };

    /** A burst playing where an item was taken: its tree's sequence, the pose it drives, and
     * the field emitters riding the tree's particle nodes. */
    struct Effect {
        const TreeInfo* figure = nullptr;
        Vec3 position{0.0f, 0.0f, 0.0f};
        AnimationPlayer player;
        TreePose pose;
        std::vector<std::pair<usize, usize>> emitters; ///< tree node, field emitter
        f32 secondsLeft = 0.0f; ///< the time left when the tree has no sequence
        bool emitting = true;
    };

    /** Stands every powerup instance of the layout whose figure an archive holds; false
     * when none could be placed. The collision, when given, sets their height. */
    bool bind(RenderDevice& device, const WorldLayout& layout, const WorldCollision* collision,
              std::span<ItemArchive* const> archives);
    void clear();
    usize size() const { return m_items.size(); }
    const Item& item(usize index) const { return m_items[index]; }
    /** Follow a container's animated socket without floor snapping. */
    void attach(usize index, const Mat4& transform, bool contained);
    usize visibleCount() const;
    s32 playerCount() const { return m_players; }

    /** Shows the items a party of `players` sees. */
    void setPlayerCount(s32 players);
    /** Removes the tower's introductory crystal pickups once the province gate is earned. */
    void retireCrystals();
    /** Poison exposed food within a gas cloud, retaining the pickup's placement and
     * identity. Returns newly changed items, for the retail gas-spoils-food message. */
    usize poisonFood(RenderDevice& device, const Vec3& position, f32 radius, f32 damage);
    struct BlastChange {
        Vec3 position{0};
        bool destroyed = false; ///< otherwise treasure was reduced to junk
    };
    /** Explosions destroy exposed food/powerups and reduce treasure to junk.
     * Quest pickups are protected; potions have a separate magic-release path. */
    std::vector<BlastChange> blast(RenderDevice& device, const Vec3& position, f32 radius,
                                   f32 damage);
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
    bool placeRecord(RenderDevice& device, s32 record, const Vec3& position,
                     const WorldCollision* collision, s32 amount = 0);
    /** Throws a pickup of the record named `name` from `position` at `velocity`: it sails
     * out, falls, bounces and rolls to a stop on the floor the collision finds (or is lost,
     * falling where there is none), and cannot be taken for `noGrabSeconds`. False as for
     * `place`. */
    bool throwItem(RenderDevice& device, std::string_view name, const Vec3& position,
                   const Vec3& velocity, const WorldCollision* collision, f32 noGrabSeconds,
                   std::optional<f32> strength = std::nullopt);
    /** Whether any gold lies untaken. */
    bool goldLeft() const;
    /** Turns the figures, flies what was thrown and plays the bursts on by `seconds`. */
    void update(f32 seconds);
    // How a thrown item flies, as the original's coins do.
    static constexpr f32 kGravity = 32.0f; ///< units a second each second
    static constexpr f32 kBounce = 0.4f;   ///< of the fall's speed, back up
    static constexpr f32 kRestHeight =
        0.1f; ///< this near the floor it touches down; a bounce that would not clear it is the last
    static constexpr f32 kAirDrag = 0.5f;    ///< of its speed sideways lost a second aloft
    static constexpr f32 kGroundDrag = 4.0f; ///< and touching down
    static constexpr f32 kThrownFloorReach =
        100.0f; ///< how far under a flying item the floor is looked for
    usize effectCount() const { return m_effects.size(); }
    const Effect& effect(usize index) const { return m_effects[index]; }
    const ParticleField& bursts() const { return m_bursts; }
    /** Hides the crystals for reveal() to bring in. */
    void hideCrystals();
    /** Moves the reveal `seconds` on: crystals within its reach fade in. */
    void reveal(f32 seconds);
    bool revealing() const { return m_revealing; }
    /** Particles alive over every burst. */
    usize burstParticleCount() const { return m_bursts.particleCount(); }
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
    bool replaceFigure(RenderDevice& device, Item& item, std::string_view name);
    bool exposedWithin(const Item& item, const Vec3& position, f32 radius) const;
    /** Flies a thrown item `seconds` on. */
    void fly(Item& item, f32 seconds);
    std::vector<ArchiveMotion> m_motions;
    const WorldCollision* m_collision = nullptr; ///< the floor thrown items land on
    s32 m_players = 0;
    f32 m_frameRemainder = 0.0f;
    f32 m_revealTime = 0.0f;
    bool m_revealing = false;
};

} // namespace gdl::game
