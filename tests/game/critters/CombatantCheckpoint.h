#pragma once

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "game/critters/Combatant.h"

namespace gdl::game {
/** Exact actor continuation, not admission to speculative combat. Stocks, collision,
 * hazards and lookout definitions are trusted borrowed resources and must outlive
 * this wrapper. No resource is loaded, model uploaded, or native event dispatched
 * while capturing/preparing/committing. Shared stock texture clocks belong to the
 * population checkpoint; this owner never advances or independently forks them. */
class CombatantCheckpoint {
public:
    static constexpr usize kMaxParts = 64;
    static constexpr usize kMaxRows = 4096;
    static constexpr usize kMaxEvents = 4096;
    struct Resource {
        u32 id = 0;
        CombatantAssets* stock = nullptr;
    };
    struct Bindings {
        const WorldCollision* collision = nullptr;
        const HazardSurfaces* hazards = nullptr;
        const LookoutRoute* lookouts = nullptr;
    };
    struct Definition {
        u32 resource = 0;
        u32 index = 0; // 0 is the root; n + 1 is stock.children[n].
        bool operator==(const Definition&) const = default;
    };
    struct Shot {
        Definition definition;
        s32 critter = -1, damageIndex = -1;
        Vec3 origin{0}, forward{0, 0, 1};
        std::optional<Vec3> target;
        f32 rate = 1, scale = 1, damageScale = 1, birthLife = 0;
        bool endVisual = false;
        char realm = 'G';
        bool operator==(const Shot&) const = default;
    };
    struct Events {
        std::vector<CombatCue> cues;
        std::vector<CombatGrab> grabs;
        std::vector<CombatSpew> spews;
        std::vector<Shot> shots;
        std::vector<CombatArenaActivation> arena;
        std::vector<CombatTrample> tramples;
        std::vector<CombatantRam> rams;
        std::vector<CombatPush> pushes;
        std::vector<CombatBlow> blows;
        std::vector<CombatLoss> losses;
        bool valid() const;
        bool operator==(const Events& /*other*/) const;
    };
    struct PlayerDamage {
        f32 dealt = 0, received = 0, dealtTime = 0, receivedTime = 0;
        bool operator==(const PlayerDamage&) const = default;
    };
    struct Target {
        s32 player = -1;
        f32 distance = 0, score = 0, inverseAnger = 1;
        Vec2 direction{0};
        bool operator==(const Target&) const = default;
    };
    struct Floor {
        s32 object = -1;
        Vec3 local{0};
        bool operator==(const Floor&) const = default;
    };
    struct HitNode {
        f32 health = 0;
        s32 flashTicks = 0;
        bool broken = false;
        std::optional<NodePose> heldPose;
        bool operator==(const HitNode&) const = default;
    };
    struct Replacement {
        u32 node = 0;
        std::string sourceNode, object; // Exact prebound replacementModels map key.
        bool operator==(const Replacement&) const = default;
    };
    struct Presentation {
        bool valid = false;
        u8 state = 0;
        u64 generation = 0;
        u32 sequence = 0;
        Vec3 position{0};
        f32 yaw = 0, frame = 0;
        std::optional<TreePose::Checkpoint> pose;
        bool operator==(const Presentation&) const = default;
    };
    struct Attachment {
        AnimationPlayer::Checkpoint player;
        std::optional<TreePose::Checkpoint> pose;
        Mat4 world{1};
        f32 previousFrame = 0;
        u64 previousGeneration = 0;
        bool operator==(const Attachment&) const = default;
    };
    struct Actor {
        u8 state = 0;
        bool hidden = false, forcedPattern = false, childrenIntact = true, smoothValid = false;
        Definition definition;
        s32 parent = -1;
        std::optional<u32> branch;
        f32 health = 0, burnGap = 0, maxHealth = 1;
        Vec3 position{0};
        std::optional<Floor> floor;
        f32 yaw = 0, initialYaw = 0;
        Vec3 homePosition{0}, initialRoot{0}, push{0};
        s32 target = -1;
        f32 targetDistance = 100000, targetYaw = 0;
        std::vector<Target> targets;
        std::array<PlayerDamage, 4> playerDamage{};
        s32 move = -1, moveTarget = -1, pattern = -1;
        f32 finishedSeconds = 0, age = 0;
        std::vector<f32> moveTimes, patternTimes;
        u32 patternStep = 0;
        std::vector<s32> struckThisMove;
        std::vector<CritterArea> areas;
        std::vector<Mat4> arenaAnchors;
        std::vector<CombatArenaTarget> arenaTargets;
        bool patrolBound = false;
        s32 lookout = -1;
        f32 sight = 0;
        s32 lastArenaTarget = -1;
        f32 hurtPending = 0;
        u32 hurtFlags = 0;
        Vec3 hurtDirection{0};
        f32 roarOwed = 0, sinceHurt = 0;
        s32 flashTicks = 0, skin = -1;
        f32 skinAge = 0;
        std::vector<HitNode> hitNodes;
        std::vector<Replacement> replacements;
        f32 alpha = 1;
        Color tint = Color::white();
        f32 scale = 1, shrink = 1;
        s32 frozenTicks = 0, blindTicks = 0;
        f32 curbSeconds = 0;
        bool held = false, roarWanted = false, moveEffect = false, moveDone = false;
        u32 soundsGiven = 0;
        bool arenaCollected = false;
        s32 shotFrame = -1;
        std::optional<Vec3> attackTarget, stepTarget, patrolAim;
        s32 grabbed = -1, grabMove = -1;
        std::string grabNode;
        Vec3 grabOffset{0};
        AnimationPlayer::Checkpoint player;
        std::optional<TreePose::Checkpoint> pose, smooth;
        Presentation presentation;
        std::vector<Attachment> attachments;
        std::array<Vec2, 2> gaze{};
        bool valid() const;
        bool operator==(const Actor& /*other*/) const;
    };
    struct Part {
        Actor actor;
        u64 incarnation = 0;
        s32 id = -1;
        bool collision = false, hazards = false;
        EnemyScales scales;
        char realm = 'G';
        bool fullHarm = false;
        CritterPressure pressure;
        /** Canonical linear-congruential state, not a standard-library object dump.
         * Native minstd_rand draws/mapping are unchanged by this local checkpoint. */
        u32 arenaRandom = 1;
        Events events;
        bool valid() const;
        bool operator==(const Part& /*other*/) const;
    };
    struct State {
        // Root first; native child actors are direct children, in authored order.
        std::vector<Part> parts;
        std::optional<LookoutRoute> lookouts;
        bool valid() const;
        bool operator==(const State&) const = default;
    };
    class Prepared {
    public:
        Prepared(Prepared&&) noexcept;
        Prepared& operator=(Prepared&&) noexcept;
        ~Prepared();
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;

    private:
        friend class CombatantCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };

    CombatantCheckpoint(Combatant& owner, std::span<const Resource> resources, Bindings bindings);
    ~CombatantCheckpoint();
    CombatantCheckpoint(const CombatantCheckpoint&) = delete;
    CombatantCheckpoint& operator=(const CombatantCheckpoint&) = delete;
    CombatantCheckpoint(CombatantCheckpoint&&) = delete;
    CombatantCheckpoint& operator=(CombatantCheckpoint&&) = delete;
    /** Post-native-phase only: Critters binds transient obstacle/swarm spans for
     * update and clears them immediately afterward. Nonempty spans refuse capture
     * without dereferencing potentially expired frame-local storage. They are not
     * retained continuation data and must be rebound by the next native update. */
    std::optional<State> checkpoint() const;
    std::optional<Prepared> prepareRestore(const State& /*state*/) const;
    bool canCommit(const Prepared& /*value*/) const;
    /** Allocation/callback-free. Native updates may occur between preparation and
     * rewind; clear/rebind or any successful restore invalidates sibling tokens. */
    bool commitRestore(Prepared&& /*value*/);
    bool restore(const State& /*state*/);
    std::optional<Events>
        captureEvents(std::span<const CombatCue> /*cues*/, std::span<const CombatGrab> /*grabs*/,
                      std::span<const CombatSpew> /*spews*/, std::span<const CombatShot> /*shots*/,
                      std::span<const CombatArenaActivation> /*arena*/,
                      std::span<const CombatTrample> /*tramples*/,
                      std::span<const CombatantRam> /*rams*/,
                      std::span<const CombatPush> /*pushes*/, std::span<const CombatBlow> /*blows*/,
                      std::span<const CombatLoss> /*losses*/) const;
    std::optional<std::vector<CombatShot>> restoreShots(std::span<const Shot> /*shots*/) const;

private:
    bool bound() const;
    const Resource* resource(u32 id) const;
    std::optional<Definition> definition(const CritterData* /*data*/) const;
    const CritterData* definition(Definition /*value*/) const;
    std::optional<Part> capturePart(const Combatant& /*owner*/, s32 parent) const;
    bool preparePart(Combatant& staged, const Part& /*part*/, Combatant* parent) const;
    static void install(Combatant& destination, Combatant& staged);
    Combatant& m_owner;
    std::vector<Resource> m_resources;
    std::vector<std::shared_ptr<const u8>> m_resourceBindings;
    Bindings m_bindings;
    std::optional<LookoutRoute> m_lookouts;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
};
} // namespace gdl::game
