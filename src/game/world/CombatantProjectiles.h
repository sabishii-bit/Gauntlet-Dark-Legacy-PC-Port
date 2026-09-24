#pragma once

#include <functional>
#include <random>
#include <span>
#include <string_view>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/core/Types.h"
#include "engine/world/WorldCollision.h"

#include "game/enemies/CombatantProjectile.h"
#include "game/enemies/Enemies.h"
#include "game/world/EffectTrees.h"

namespace gdl::game {
struct CombatantProjectileHit {
    s32 player = -1;
    f32 damage = 0.0f;
    u32 flags = 0;
    Vec3 direction{0.0f}; ///< direction of travel at contact
    f32 repeatGap = 0.0f; ///< shared player effect immunity after damage above two
};

/** Moving attack effects, independent of the creature's animation after launch.
 * Archives and attack tables are borrowed until clear(). Effects must outlive this object.
 * Stage generators and grab/attached-area attacks are separate from this projectile path. */
class CombatantProjectiles {
public:
    using PlaySound = std::function<void(std::string_view)>;
    void launch(const CombatShot& shot, ItemArchive& archive, RenderDevice& device,
                EffectTrees& effects, const PlaySound& sound);
    void update(f32 seconds, const WorldCollision* collision, std::span<const EnemyView> players,
                RenderDevice& device, EffectTrees& effects, const PlaySound& sound);
    void clear(EffectTrees& effects);
    std::vector<CombatantProjectileHit> takeHits();
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
        bool settled = false; ///< generator projectile's impact is finishing before placement
        bool leavesGenerator = false;
        bool summonsEnemies = false;
        f32 contactSeconds = 0; ///< sticky contacts advance on the authored 30 Hz game clock
        s32 piercedPlayer = -1; ///< reflecting shots spend their pass-through on first contact
    };
    u32 show(Flying& flying, s32 index, RenderDevice& device, EffectTrees& effects,
             const PlaySound& sound, f32 life = 0.0f);
    static void place(const Flying& flying, EffectTrees& effects);
    void summon(Flying& flying);
    std::vector<Flying> m_flying;
    std::vector<u32> m_emittedEffects; ///< impacts and end effects still borrow the launch archive
    std::vector<CombatantProjectileHit> m_hits;
    std::vector<Mat4> m_generators;
    std::vector<Mat4> m_summons;
    std::mt19937 m_random;
};
} // namespace gdl::game
