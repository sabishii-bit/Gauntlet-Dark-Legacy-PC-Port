#pragma once

#include <functional>
#include <random>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/world/WorldCollision.h"

#include "game/enemies/CombatantKind.h"
#include "game/enemies/CombatantProjectile.h"
#include "game/enemies/CritterArea.h"
#include "game/enemies/Enemies.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/world/EffectTrees.h"

namespace gdl::game {
struct CombatantProjectileHit {
    s32 player = -1;
    f32 damage = 0.0f;
    u32 flags = 0;
    Vec3 direction{0.0f}; ///< direction of travel at contact
    f32 repeatGap = 0.0f; ///< shared player effect immunity after damage above two
    s32 critter = -1;
    CombatantKind ownerKind = CombatantKind::Unknown;
};

/** A world impact, including a rebound that does not end the projectile. */
struct CombatantWorldHit {
    Vec3 position{0};
    s32 object = -1;
    bool splash = false; ///< only a terminating liquid hit plays the water sound
};

/** Launched attacks and planted traps, independent of their creature's animation.
 * Archives and attack tables are borrowed until clear(). Effects must outlive this object.
 * Capturing and attached-area attacks are separate from this path. */
class CombatantProjectiles {
public:
    using PlaySound = std::function<void(std::string_view)>;
    void launch(const CombatShot& shot, ItemArchive& archive, RenderDevice& device,
                EffectTrees& effects, const PlaySound& sound,
                const WorldCollision* collision = nullptr);
    void update(f32 seconds, const WorldCollision* collision, std::span<const EnemyView> players,
                RenderDevice& device, EffectTrees& effects, const PlaySound& sound,
                std::span<const MissileStop> items = {});
    void clear(EffectTrees& effects);
    std::vector<CombatantProjectileHit> takeHits();
    std::vector<RockHit> takeRockHits() { return std::exchange(m_rockHits, {}); }
    std::vector<CombatantWorldHit> takeWorldHits() { return std::exchange(m_worldHits, {}); }
    /** Expired SFXX 0x20000 effects leave a stage-owned BOSSGEN at this placement. */
    std::vector<Mat4> takeGenerators();
    /** SFXX 0x400000 invokes the boss summon callback on collision or expiration. */
    std::vector<Mat4> takeSummons();
    usize count() const { return m_flying.size(); }

private:
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
    };
    struct ItemImpact {
        f32 fraction = 0;
        bool suppressEffect = false; ///< piercing shots stopped by surviving cover
    };
    std::optional<ItemImpact> hitItems(const Flying& flying, const Vec3& from, const Vec3& to,
                                       f32 limit, std::span<const MissileStop> items);
    u32 show(Flying& flying, s32 index, RenderDevice& device, EffectTrees& effects,
             const PlaySound& sound, f32 life = 0.0f);
    static void place(const Flying& flying, EffectTrees& effects);
    void stickyContacts(Flying& flying, f32 seconds, std::span<const EnemyView> players);
    /** Places the impact (even a purely visual one); true if it owns ongoing area damage. */
    static bool settleImpact(Flying& flying, u32 effect, EffectTrees& effects,
                             const WorldCollision* collision);
    void impactContacts(Flying& flying, f32 seconds, std::span<const EnemyView> players);
    void summon(Flying& flying);
    std::vector<Flying> m_flying;
    std::vector<u32> m_emittedEffects; ///< impacts and end effects still borrow the launch archive
    std::vector<CombatantProjectileHit> m_hits;
    std::vector<CombatantWorldHit> m_worldHits;
    std::vector<RockHit> m_rockHits;
    std::vector<Mat4> m_generators;
    std::vector<Mat4> m_summons;
    std::mt19937 m_random;
};
} // namespace gdl::game
