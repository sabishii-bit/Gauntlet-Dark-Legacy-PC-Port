#pragma once

#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/math/Mt19937.h"
#include "engine/world/WorldCollision.h"

#include "game/critters/CombatantKind.h"
#include "game/critters/CombatantProjectile.h"
#include "game/critters/CritterArea.h"
#include "game/effects/EffectTrees.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/level/MissileStop.h"

namespace gdl::game {
struct CombatantProjectileHit {
    s32 player = -1;
    f32 damage = 0.0f;
    u32 flags = 0;
    Vec3 direction{0.0f}; ///< direction of travel at contact
    f32 repeatGap = 0.0f; ///< shared player effect immunity after damage above two
    s32 critter = -1;
    CombatantKind ownerKind = CombatantKind::Unknown;
    s32 target = -1; ///< a reflected missile's opponent, in the caller's target namespace
    bool ricochet = false;
    Vec3 position{0};
    u64 instance = 0; ///< retained flight birth, independent of the shooter's recycled slot
    bool operator==(const CombatantProjectileHit&) const = default;
    friend void visitNativeState(NativeStateDigest& digest, const CombatantProjectileHit& state) {
        digest.fields<11>(state);
    }
};

/** A world impact, including a rebound that does not end the projectile. */
struct CombatantWorldHit {
    Vec3 position{0};
    s32 object = -1;
    bool splash = false; ///< only a terminating liquid hit plays the water sound
    u64 instance = 0;
    bool operator==(const CombatantWorldHit&) const = default;
    friend void visitNativeState(NativeStateDigest& digest, const CombatantWorldHit& state) {
        digest.fields<4>(state);
    }
};

/** Launched attacks and planted traps, independent of their creature's animation.
 * Archives and attack tables are borrowed until clear(). Effects must outlive this object.
 * Capturing and attached-area attacks are separate from this path. */
class CombatantProjectiles {
public:
    CombatantProjectiles() = default;
    CombatantProjectiles(const CombatantProjectiles&) = delete;
    CombatantProjectiles& operator=(const CombatantProjectiles&) = delete;
    CombatantProjectiles(CombatantProjectiles&&) = delete;
    CombatantProjectiles& operator=(CombatantProjectiles&&) = delete;
    ~CombatantProjectiles() = default;
    enum class SoundPhase : u32 { Launch = 1, Morph, Impact, End, Splash, Ricochet };
    struct SoundCue {
        u64 instance = 0;
        SoundPhase phase = SoundPhase::Launch;
        s32 effect = -1;       ///< authored SFXX row, including linked cues
        std::string_view name; ///< borrowed only for the synchronous callback
        CombatantKind ownerKind = CombatantKind::Unknown;
        Vec3 position{0};
    };
    using PlaySound = std::function<void(const SoundCue&)>;
    void launch(const CombatShot& shot, ItemArchive& archive, RenderDevice& device,
                EffectTrees& effects, const PlaySound& sound,
                const WorldCollision* collision = nullptr);
    void update(f32 seconds, const WorldCollision* collision, std::span<const EnemyView> players,
                RenderDevice& device, EffectTrees& effects, const PlaySound& sound,
                std::span<const MissileStop> items = {},
                std::span<const MissileTarget> opponents = {});
    void clear(EffectTrees& effects);
    std::vector<CombatantProjectileHit> takeHits();
    std::vector<RockHit> takeRockHits() { return std::exchange(m_sim.rockHits, {}); }
    std::vector<CombatantWorldHit> takeWorldHits() { return std::exchange(m_sim.worldHits, {}); }
    /** Expired SFXX 0x20000 effects leave a stage-owned BOSSGEN at this placement. */
    std::vector<Mat4> takeGenerators();
    /** SFXX 0x400000 invokes the boss summon callback on collision or expiration. */
    std::vector<Mat4> takeSummons();
    usize count() const { return m_sim.flying.size(); }

private:
    friend class OpponentCombatCheckpoint;
    friend class CombatantProjectilesCheckpoint;
    struct Flying {
        CombatShot shot;
        ItemArchive* archive = nullptr;
        Vec3 position{0.0f};
        Vec3 velocity{0.0f};
        Vec3 spin{0.0f};
        Vec3 rotation{0.0f};
        u32 effect = 0;
        bool morphed = false;
        bool stuck = false;   ///< stationary sticky impact, no longer a flying missile
        bool planted = false; ///< stationary DAMG area, with birth/hold/end phases
        f32 phaseSeconds = 0;
        bool settled = false; ///< stationary impact, optionally followed by generator placement
        f32 impactRadius = 0; ///< live damageradius; the final flight morph clears it
        std::optional<CritterArea> impactArea; ///< expanding damage after the flight ends
        bool leavesGenerator = false;
        bool summonsEnemies = false;
        f32 contactSeconds = 0; ///< sticky contacts advance on the authored 30 Hz game clock
        s32 piercedPlayer = -1; ///< reflecting shots spend their pass-through on first contact
        bool reflected = false;
        u64 instance = 0;
        bool operator==(const Flying&) const = default;
        friend void visitNativeState(NativeStateDigest& digest, const Flying& state) {
            const auto& [shot, archive, position, velocity, spin, rotation, effect, morphed, stuck,
                         planted, phaseSeconds, settled, impactRadius, impactArea, leavesGenerator,
                         summonsEnemies, contactSeconds, piercedPlayer, reflected, instance] =
                state;
            digest.value(shot);
            digest.resource(archive);
            digest.values(position, velocity, spin, rotation, effect, morphed, stuck, planted,
                          phaseSeconds, settled, impactRadius, impactArea, leavesGenerator,
                          summonsEnemies, contactSeconds, piercedPlayer, reflected, instance);
        }
    };
    struct ItemImpact {
        f32 fraction = 0;
        bool suppressEffect = false; ///< piercing shots stopped by surviving cover
    };
    std::optional<ItemImpact> hitItems(const Flying& flying, const Vec3& from, const Vec3& to,
                                       f32 limit, std::span<const MissileStop> items);
    u32 show(Flying& flying, s32 index, SoundPhase phase, RenderDevice& device,
             EffectTrees& effects, const PlaySound& sound, f32 life = 0.0f);
    static void place(const Flying& flying, EffectTrees& effects);
    void stickyContacts(Flying& flying, f32 seconds, std::span<const EnemyView> players);
    /** Places the impact (even a purely visual one); true if it owns ongoing area damage. */
    static bool settleImpact(Flying& flying, u32 effect, EffectTrees& effects,
                             const WorldCollision* collision);
    void impactContacts(Flying& flying, f32 seconds, std::span<const EnemyView> players);
    void summon(Flying& flying);
    struct Simulation {
        std::vector<Flying> flying;
        std::vector<u32>
            emittedEffects; ///< impacts and end effects still borrow the launch archive
        std::vector<CombatantProjectileHit> hits;
        std::vector<CombatantWorldHit> worldHits;
        std::vector<RockHit> rockHits;
        std::vector<Mat4> generators;
        std::vector<Mat4> summons;
        Mt19937 random;
        f32 ricochetIn = 0;
        u64 nextInstance = 1; ///< zero means exhausted; clear never reuses an issued birth
        bool operator==(const Simulation&) const = default;
        friend void visitNativeState(NativeStateDigest& digest, const Simulation& state) {
            digest.fields<10>(state);
        }
    };
    Simulation m_sim;

public:
    class LocalSnapshot {
    private:
        friend class CombatantProjectiles;
        Simulation m_state;
        std::weak_ptr<const u8> m_owner;
        u64 m_epoch = 0;
    };
    /** Native value history. Borrowed specs, archives and attack tables must
     * remain alive for this stage epoch.
     * The caller also rewinds the shared EffectTrees once with this state. */
    void saveLocal(LocalSnapshot& state) const;
    bool canRestoreLocal(const LocalSnapshot& state) const;
    bool restoreLocal(const LocalSnapshot& state);
    bool equalLocal(const LocalSnapshot& a, const LocalSnapshot& b) const;
    bool appendNativeDigest(NativeStateDigest& digest) const;
    /** Foreign/stale history is refused before appending any bytes. */
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& state) const;

private:
    /** Clear or a successful checkpoint restore invalidates prepared borrowers. */
    std::shared_ptr<const u8> m_checkpointBinding = std::make_shared<const u8>(0);
    std::shared_ptr<const u8> m_localOwner = std::make_shared<const u8>(0);
    u64 m_localEpoch = 1;
    u64 m_checkpointRevision = 1;
};
} // namespace gdl::game
