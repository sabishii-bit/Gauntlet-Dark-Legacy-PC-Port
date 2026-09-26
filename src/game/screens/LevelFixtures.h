#pragma once
#include <functional>
#include <optional>
#include <span>
#include <string_view>

#include "engine/core/Types.h"

#include "game/screens/PlayerHealth.h"
#include "game/world/Breakables.h"
#include "game/world/Chests.h"
#include "game/world/EffectTrees.h"
#include "game/world/LevelSoundscape.h"
#include "game/world/LevelWorld.h"
#include "game/world/LockedGates.h"
#include "game/world/SafeRocks.h"
#include "game/world/Traps.h"
namespace gdl::game {
/** Owns interactive scenery and its hazard chains. Scene services are borrowed while bound.
 * Events are synchronous so health, help and opponent damage retain their original order.
 * No player span or callback is retained. Clear before world/archive/device teardown. */
class LevelFixtures {
public:
    static constexpr f32 kBlastRadius = 12.0f;
    struct Resources {
        RenderDevice& device;
        LevelWorld& world;
        ItemArchive& weapons;
        EffectTrees& effects;
        LevelSoundscape& audio;
        f32 difficultyGain = 1;
    };
    struct Events {
        std::function<void(usize, f32, HurtKind, bool)> hurt;
        std::function<void(s32, usize)> help;
        std::function<void(s32, std::string_view)> card;
        std::function<void(const Vec3&, f32, f32)> opponents;
        std::function<bool(s32, const Vec3&, s32)> releaseEnemy;
        std::function<void(s32, const Vec3&)> shatterPotion;
    };
    void bind(const Resources& resources);
    void clear();
    void setPlayerCount(s32 count);
    std::vector<Obstacle> obstacles() const;
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const CameraFrame* camera = nullptr) const;
    void update(s32 ticks, f32 seconds, std::span<PlayerRuntime> players, const Events& events);
    void strikeSafeRock(usize index, f32 power);
    void strikeWall(usize index, f32 power, u32 flags = 0);
    void strikeBarrel(usize barrel, f32 power, s32 byPlayer, std::span<PlayerRuntime> players,
                      const Events& events);
    void blast(const Vec3& position, f32 radius, f32 damage, std::span<PlayerRuntime> players,
               const Events& events);
    void settleBlasts(std::span<PlayerRuntime> players, const Events& events);
    Chests& chests() { return m_chests; }
    const Chests& chests() const { return m_chests; }
    LockedGates& gates() { return m_gates; }
    const LockedGates& gates() const { return m_gates; }
    const Traps& traps() const { return m_traps; }
    const Breakables& barrels() const { return m_barrels; }
    const SafeRocks& safeRocks() const { return m_safeRocks; }
    SafeRocks& safeRocks() { return m_safeRocks; }

private:
    void updateClouds(f32 seconds, std::span<PlayerRuntime> players, const Events& events);
    void playGateSound(s32 subtype);
    void playRealmSound(std::string_view stem);
    f32 trapDamageScale() const;
    std::optional<Resources> m_resources;
    /** Gas a poison barrel left hanging. */
    struct GasCloud {
        Vec3 position{0.0f, 0.0f, 0.0f};
        f32 damage = 0.0f;
        f32 secondsLeft = 0.0f;
        u32 effect = 0;
    };
    std::vector<GasCloud> m_clouds;
    /** A blast yet to be felt: one barrel's sets off the next, in turn. */
    struct Blast {
        Vec3 position{0.0f, 0.0f, 0.0f};
        f32 radius = 0.0f;
        f32 damage = 0.0f;
    };
    std::vector<Blast> m_blasts;

    Chests m_chests;
    LockedGates m_gates;
    Traps m_traps;
    Breakables m_barrels;
    SafeRocks m_safeRocks;
};
} // namespace gdl::game
