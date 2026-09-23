#pragma once
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

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
    static constexpr float kBlastRadius = 12.0f;
    struct Resources {
        RenderDevice& device;
        LevelWorld& world;
        ItemArchive& weapons;
        EffectTrees& effects;
        LevelSoundscape& audio;
        float difficultyGain = 1;
    };
    struct Events {
        std::function<void(std::size_t, float, HurtKind, bool)> hurt;
        std::function<void(int, std::size_t)> help;
        std::function<void(int, std::string_view)> card;
        std::function<void(const Vec3&, float, float)> opponents;
    };
    void bind(const Resources& resources);
    void clear();
    void setPlayerCount(int count);
    std::vector<Obstacle> obstacles() const;
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;
    void update(int ticks, float seconds, std::span<PlayerRuntime> players, const Events& events);
    void strikeSafeRock(std::size_t index, float power);
    void strikeBarrel(std::size_t barrel, float power, int byPlayer,
                      std::span<PlayerRuntime> players, const Events& events);
    void blast(const Vec3& position, float radius, float damage, std::span<PlayerRuntime> players,
               const Events& events);
    void settleBlasts(std::span<PlayerRuntime> players, const Events& events);
    Chests& chests() { return m_chests; }
    const Chests& chests() const { return m_chests; }
    LockedGates& gates() { return m_gates; }
    const LockedGates& gates() const { return m_gates; }
    const Traps& traps() const { return m_traps; }
    const Breakables& barrels() const { return m_barrels; }
    const SafeRocks& safeRocks() const { return m_safeRocks; }

private:
    void updateClouds(float seconds, std::span<PlayerRuntime> players, const Events& events);
    void playGateSound(int subtype);
    void playRealmSound(std::string_view stem);
    float trapDamageScale() const;
    std::optional<Resources> m_resources;
    /** Gas a poison barrel left hanging. */
    struct GasCloud {
        Vec3 position{0.0f, 0.0f, 0.0f};
        float damage = 0.0f;
        float secondsLeft = 0.0f;
    };
    std::vector<GasCloud> m_clouds;
    /** A blast yet to be felt: one barrel's sets off the next, in turn. */
    struct Blast {
        Vec3 position{0.0f, 0.0f, 0.0f};
        float radius = 0.0f;
        float damage = 0.0f;
    };
    std::vector<Blast> m_blasts;

    Chests m_chests;
    LockedGates m_gates;
    Traps m_traps;
    Breakables m_barrels;
    SafeRocks m_safeRocks;
};
} // namespace gdl::game
