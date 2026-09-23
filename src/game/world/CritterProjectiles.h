#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <random>
#include <span>
#include <string_view>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/world/WorldCollision.h"

#include "game/enemies/CritterProjectile.h"
#include "game/enemies/Enemies.h"
#include "game/world/EffectTrees.h"

namespace gdl::game {
struct CritterProjectileHit {
    std::int32_t player = -1;
    float damage = 0.0f;
    std::uint32_t flags = 0;
};

/** Moving attack effects, independent of the creature's animation after launch.
 * Archives and attack tables are borrowed until clear(). Effects must outlive this object.
 * Stage generators and grab/attached-area attacks are separate from this projectile path. */
class CritterProjectiles {
public:
    using PlaySound = std::function<void(std::string_view)>;
    void launch(const CritterShot& shot, ItemArchive& archive, RenderDevice& device,
                EffectTrees& effects, const PlaySound& sound);
    void update(float seconds, const WorldCollision* collision, std::span<const EnemyView> players,
                RenderDevice& device, EffectTrees& effects, const PlaySound& sound);
    void clear(EffectTrees& effects);
    std::vector<CritterProjectileHit> takeHits();
    std::size_t count() const { return m_flying.size(); }

private:
    struct Flying {
        CritterShot shot;
        ItemArchive* archive = nullptr;
        Vec3 position{0.0f};
        Vec3 velocity{0.0f};
        Vec3 spin{0.0f};
        Vec3 rotation{0.0f};
        std::uint32_t effect = 0;
        bool morphed = false;
    };
    static std::uint32_t show(Flying& flying, std::int32_t index, RenderDevice& device,
                              EffectTrees& effects, const PlaySound& sound, float life = 0.0f);
    static void place(const Flying& flying, EffectTrees& effects);
    std::vector<Flying> m_flying;
    std::vector<CritterProjectileHit> m_hits;
    std::mt19937 m_random;
};
} // namespace gdl::game
