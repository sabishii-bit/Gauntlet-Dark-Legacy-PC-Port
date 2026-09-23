#pragma once
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TreePose.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/enemies/CombatEvents.h"
#include "game/enemies/CombatantAssets.h"
#include "game/enemies/CombatantProjectile.h"
#include "game/enemies/CritterArea.h"
#include "game/enemies/Enemies.h"

namespace gdl::game {
/** One fighter. Executes authored moves with family policy, without owning a population.
 * Assets are borrowed and must outlive this actor and any emitted projectiles/effects. */
class Combatant {
public:
    Combatant() = default;
    Combatant(const Combatant&) = delete;
    Combatant& operator=(const Combatant&) = delete;
    Combatant(Combatant&&) = delete;
    Combatant& operator=(Combatant&&) = delete;
    ~Combatant() = default;
    static constexpr f32 kBlockShare = 0.25f;
    static constexpr f32 kKillShare = 0.2f;
    static constexpr f32 kRoarAfter = 50.0f;
    static constexpr f32 kUnderLevelLoss = 0.02f;
    static constexpr s32 kTicksPerSecond = 60;
    /** Replaces the actor state; pending events survive until taken or explicitly cleared. */
    bool spawn(CombatantAssets& stock, s32 id, const Vec3& position, f32 yaw,
               const WorldCollision* collision, const EnemyScales& scales, char realm);
    void clear();
    /** Static stage attachment points supplied by the encounter, not player targets. */
    void setArenaAnchors(std::span<const Mat4> anchors);
    void setArenaTargets(std::span<const CombatArenaTarget> targets);
    bool raisesArenaRocks() const;
    void update(s32 ticks, f32 seconds, std::span<const EnemyView> players,
                std::span<const Combatant> peers = {});
    void hurt(const EnemyHit& hit);
    void freeze(s32 ticks);
    void blind(s32 ticks);
    void curb(f32 seconds);
    void resize(f32 scale);
    void hold(bool held);
    void roar();
    bool present() const { return m_actor.state != State::Inactive; }
    bool alive() const { return m_actor.state == State::Active; }
    bool dying() const { return m_actor.state == State::Dying; }
    s32 id() const { return m_id; }
    CombatantKind kind() const {
        return data() != nullptr ? data()->kind() : CombatantKind::Unknown;
    }
    f32 health() const { return m_actor.health; }
    f32 maxHealth() const { return m_actor.maxHealth; }
    const Vec3& position() const { return m_actor.position; }
    f32 yaw() const { return m_actor.yaw; }
    f32 radius() const { return data() != nullptr ? data()->radius() : 0; }
    s32 target() const { return m_actor.target; }
    bool moveDone() const { return m_actor.moveDone; }
    bool frozen() const { return m_actor.frozenTicks > 0; }
    bool blinded() const { return m_actor.blindTicks > 0; }
    bool curbed() const { return m_actor.curbSeconds > 0; }
    f32 scale() const { return m_actor.scale; }
    s32 moveType() const;
    std::string_view moveName() const;
    std::string form() const;
    const CritterData* data() const;
    ItemArchive* archive();
    std::optional<Mat4> nodeTransform(std::string_view node) const;
    std::optional<Mat4> rootTransform() const;
    std::optional<f32> contactDistance(const Vec3& from, const Vec3& to, f32 radius) const;
    bool within(const Vec3& centre, f32 radius) const;
    bool reachedBy(const Vec3& centre, f32 radius, f32 arc, const Vec3& facing) const;
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const Texture* frozenTexture = nullptr) const;
    std::vector<CombatBlow> takeBlows();
    std::vector<CombatLoss> takeLosses();
    std::vector<CombatCue> takeCues();
    std::vector<CombatSpew> takeSpews();
    std::vector<CombatShot> takeShots();
    std::vector<CombatArenaActivation> takeArenaActivations();

private:
    enum class State : u8 { Inactive, Active, Dying };
    struct Actor {
        State state = State::Inactive;
        CombatantAssets* stock = nullptr;
        f32 health = 0.0f;
        f32 maxHealth = 1.0f;
        Vec3 position{0.0f, 0.0f, 0.0f};
        f32 yaw = 0.0f;
        f32 initialYaw = 0.0f;
        Vec3 homePosition{0.0f}; ///< floor-space home anchor, independent of current position
        Vec3 initialRoot{0.0f};  ///< geometry's saved base, distinct from an explicit roaming home
        Vec3 push{0.0f, 0.0f, 0.0f};
        s32 target = -1;
        f32 targetDistance = 100000.0f;
        s32 move = -1; ///< the move playing
        bool moveDone = false;
        f32 finishedSeconds = 0.0f;
        f32 age = 0.0f;
        std::vector<f32> moveTimes;
        std::vector<f32> patternTimes;
        s32 pattern = -1;
        usize patternStep = 0;
        std::vector<f32> cooldowns;      ///< seconds left before each move may be chosen again
        std::vector<s32> struckThisMove; ///< players already hurt by the move playing
        std::vector<CritterArea> areas;
        std::vector<Mat4> arenaAnchors;
        std::vector<CombatArenaTarget> arenaTargets;
        f32 hurtPending = 0.0f;
        u32 hurtFlags = 0;
        Vec3 hurtDirection{0.0f, 0.0f, 0.0f};
        f32 roarOwed = 0.0f; ///< damage taken toward the next roar
        f32 alpha = 1.0f;
        f32 scale = 1.0f;
        s32 frozenTicks = 0;     ///< a legend item's: it stands still this long
        s32 blindTicks = 0;      ///< and finds no one this long
        f32 curbSeconds = 0.0f;  ///< over nought, its curbed attacks are refused
        bool held = false;       ///< keeps to its stance between moves
        bool roarWanted = false; ///< roars as soon as it may
        u32 soundsGiven = 0;     ///< bits: the move's sound, its second, each strike's
        s32 shotFrame = -1;
        std::optional<Vec3> attackTarget; ///< captured by a targeted-area move, not a homing point
        AnimationPlayer player;
        TreePose pose;
    };

    static bool startMove(Actor& critter, usize index, bool recordUse = true);
    static void chooseMove(Actor& critter, std::span<const EnemyView> players);
    static std::optional<usize> bestMove(const Actor& critter, std::span<const EnemyView> players);
    static bool choosePatternAttack(Actor& critter, std::span<const EnemyView> players);
    static s32 attackTarget(const Actor& critter, const TargetCriteria& criteria,
                            std::span<const EnemyView> players);
    static f32 attackRate(const Actor& critter);
    static bool supportsArea(const AttackDefinition& damage, const CombatEffectDefinition* sound);
    std::optional<f32> startArea(Actor& critter, s32 id, const AttackDefinition& damage,
                                 std::string_view node,
                                 std::optional<Mat4> worldParent = std::nullopt);
    void eruptArena(Actor& critter, s32 id, const AttackDefinition& damage,
                    std::span<const EnemyView> players);
    void updateAreas(Actor& critter, s32 id, std::span<const EnemyView> players);
    /** Whether a legend item's curb keeps the move from it. */
    static bool curbedMove(const Actor& critter, const MoveDefinition& move);
    /** Sets off sound record `index` (and what it links to) at `position`. */
    void cue(const Actor& critter, s32 id, s32 index, const Vec3& position,
             std::optional<std::string_view> node = std::nullopt);

    std::vector<CombatCue> m_cues;
    std::vector<CombatSpew> m_spews;
    std::vector<CombatShot> m_shots;
    std::vector<CombatArenaActivation> m_arenaActivations;
    void shoot(const Actor& critter, s32 id, const MoveDefinition& move, s32 damageIndex,
               std::span<const EnemyView> players);
    void strikeWith(Actor& critter, s32 id, const MoveDefinition& move, s32 damageIndex,
                    std::span<const EnemyView> players);
    static Vec3 partPosition(const Actor& critter, std::string_view node);
    static Mat4 partTransform(const Actor& critter, std::string_view node);
    static Mat4 attachmentTransform(const Actor& critter, std::string_view node);
    static Mat4 modelTransform(const Actor& critter);
    void carry(Actor& critter, f32 seconds, const MoveDefinition* move,
               std::span<const EnemyView> players, std::span<const Combatant> peers);
    static void chooseTarget(Actor& critter, std::span<const EnemyView> players);
    static const EnemyView* viewOf(std::span<const EnemyView> players, s32 player);

    Actor m_actor;
    s32 m_id = -1;
    const WorldCollision* m_collision = nullptr;
    EnemyScales m_scales;
    char m_realm = 'G';
    std::vector<CombatBlow> m_blows;
    std::vector<CombatLoss> m_losses;
};
} // namespace gdl::game
