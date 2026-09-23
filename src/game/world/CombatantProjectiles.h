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
    };
    u32 show(Flying& flying, s32 index, RenderDevice& device, EffectTrees& effects,
             const PlaySound& sound, f32 life = 0.0f);
    static void place(const Flying& flying, EffectTrees& effects);
    std::vector<Flying> m_flying;
    std::vector<u32> m_emittedEffects; ///< impacts and end effects still borrow the launch archive
    std::vector<CombatantProjectileHit> m_hits;
    std::mt19937 m_random;
};
} // namespace gdl::game
