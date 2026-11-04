#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "game/critters/CombatantProjectilesCheckpoint.h"
#include "game/critters/CritterStatues.h"
#include "game/critters/CrittersCheckpoint.h"
#include "game/effects/EffectTrees.h"
#include "game/enemies/EnemyMissiles.h"

namespace gdl::game {

class Combatant;
class LevelOpponents;

/** The ordinary population adapter's gameplay state, outside the independently
 * restored enemy/generator/missile pools. Restoring never consumes rewards,
 * plays sounds, creates effects, or advances the native phase. */
class OpponentCombatCheckpoint {
public:
    struct Reward {
        s32 player = -1;
        s32 experience = 0;
        bool destroyed = false;
        bool operator==(const Reward&) const = default;
    };
    struct State {
        static constexpr usize kMaxRecords = 65535;
        struct FollowingEffect {
            u32 effect = 0;
            s32 critter = -1;
            bool ofBoss = false;
            Vec3 offset{0};
            std::optional<std::string> node;
            Vec3 nodeOffset{0};
            bool rootAttachment = false;
            Vec2 pitchYaw{0};
            std::optional<s32> playerAttachment;
            bool operator==(const FollowingEffect&) const = default;
        };
        struct MoveEffect {
            u32 effect = 0;
            s32 critter = -1;
            bool ofBoss = false;
            bool operator==(const MoveEffect&) const = default;
        };
        struct Ram {
            s32 id = -1;
            f32 damage = 0;
            bool operator==(const Ram&) const = default;
        };
        struct Bag {
            std::optional<usize> item;
            std::string name;
            Vec3 position{0};
            f32 velocity = 20;
            f32 age = 0;
            f32 pitch = 0;
            f32 spin = 0;
            u32 effect = 0;
            bool landed = false;
            bool operator==(const Bag&) const = default;
        };
        State();
        std::array<s32, 4> hitStreak{};
        std::vector<std::pair<s32, s32>> lessons;
        std::vector<Reward> generatorRewards;
        std::vector<RockHit> rockHits;
        std::vector<Vec3> hearers;
        /** Expired handles remain here until the native cue cleanup phase. */
        std::vector<u32> cueEffects;
        f32 shrink = 1;
        u32 bagSeed = 1;
        CritterStatues::Checkpoint statues;
        /** Full population and independent launched attacks. The population
         * alone owns each shared stock texture clock. */
        CrittersCheckpoint::State critters;
        CombatantProjectilesCheckpoint::State projectiles;
        std::array<f32, 4> critterExperienceOwed{};
        std::vector<FollowingEffect> critterEffects;
        std::vector<MoveEffect> moveEffects;
        std::vector<Ram> barrelRams;
        /** Pickup indices, not input slots or live critter identities. A retired
         * critter can retain its cargo until native loss settlement. */
        std::array<std::optional<usize>, Critters::kMost> carried{};
        std::vector<Bag> bags;
        bool valid() const;
        /** Prior dormant-only simulation domain. Complete capture is not
         * permission to advance or dispatch any of these additional owners. */
        bool dormant() const;
        bool references(u8 playerMask, const EffectTrees::PoolCheckpoint& effects) const;
        /** Validate against the incoming stage's complete pickup list, never
         * the current owner's list before that stage has been restored. */
        bool pickupReferences(usize count) const;
        bool operator==(const State&) const = default;

    private:
        friend class OpponentCombatCheckpoint;
        bool validAggregate(std::span<const s32> observedBossParts = {},
                            usize observedBossResources = 0) const;
    };

    class Prepared {
    public:
        Prepared(Prepared&&) noexcept;
        Prepared& operator=(Prepared&&) noexcept;
        ~Prepared();
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;

    private:
        friend class OpponentCombatCheckpoint;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };

    /** The owner, its borrowed archives, and shared effect pool must outlive this
     * wrapper and every preparation. Close/rebind invalidates existing plans;
     * restoring the separate population pools does not. */
    explicit OpponentCombatCheckpoint(LevelOpponents& owner,
                                      std::span<const EffectTrees::PoolResource> resources = {});
    OpponentCombatCheckpoint(const OpponentCombatCheckpoint&) = delete;
    OpponentCombatCheckpoint& operator=(const OpponentCombatCheckpoint&) = delete;
    OpponentCombatCheckpoint(OpponentCombatCheckpoint&&) = delete;
    OpponentCombatCheckpoint& operator=(OpponentCombatCheckpoint&&) = delete;
    ~OpponentCombatCheckpoint();

    /** Refuses uncaptured boss and Death owners, including their
     * pending outputs. The enclosing bundle restores the supplied shared effect
     * pool once. Capturing an ordinary critter does not authorize dispatch of
     * grabs, drops, arena callbacks or unprepared effects. */
    std::optional<State> checkpoint(const EffectTrees::PoolCheckpoint& effects) const;
    std::optional<Prepared> prepareRestore(const State& state,
                                           const EffectTrees::PoolCheckpoint& effects) const;
    bool canCommit(const Prepared& prepared) const;
    /** Allocation-free and callback-free after successful preflight. No other
     * participant may mutate the bound level between preflight and commit. */
    bool commitRestore(Prepared&& prepared);
    bool restore(const State& state, const EffectTrees::PoolCheckpoint& effects);

private:
    struct CaptureResources;
    static bool unused(const Combatant& actor);
    bool supported() const;
    /** Local observation may include boss-owned effects and projectile stocks,
     * checked against this owner's live part/resource roster. That observation
     * is not an ordinary restore point: valid/checkpoint/prepareRestore retain
     * their narrower domain, and the boss's own state is not captured here. */
    std::optional<State> captureValues(const EffectTrees::PoolCheckpoint& effects,
                                       bool observeBoss = false) const;
    LevelOpponents& m_owner;
    std::vector<EffectTrees::PoolResource> m_effectResources;
    mutable std::unique_ptr<CaptureResources> m_captureResources;
    u64 m_generation = 0;
    std::shared_ptr<const u8> m_lifetime = std::make_shared<const u8>(0);
};

} // namespace gdl::game
