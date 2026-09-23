#pragma once
#include <cstddef>
#include <functional>
#include <optional>
#include <span>

#include "engine/world/AmbientDimmer.h"

#include "game/screens/LevelFixtures.h"
#include "game/screens/LevelOpponents.h"
#include "game/world/MoveStrikes.h"
#include "game/world/PlayerArsenal.h"
namespace gdl::game {
/** Resolves player attacks against level targets and owns their transient effects.
 * Arsenal owns projectile models; TurboMove owns per-player timelines. This component
 * connects animation actions to world hits, with no scene or retained party span.
 * Clear before releasing its borrowed effect store, arsenal, figures or archives. */
class PlayerAttacks {
public:
    struct Resources {
        RenderDevice& device;
        const ClassDataSet& classes;
        LevelWorld& world;
        ItemArchive& weapons;
        EffectTrees& effects;
        LevelSoundscape& audio;
        SoundPlayer* sounds;
        PlayerArsenal& arsenal;
        AmbientDimmer& dimmer;
    };
    struct Targets {
        LevelOpponents& opponents;
        LevelFixtures& fixtures;
        LevelFixtures::Events fixtureEvents;
    };
    void bind(const Resources& resources);
    void clear();
    void ramBarrels(std::size_t index, std::span<PlayerRuntime> players, const Targets& targets);
    void shieldPotion(std::size_t index, std::span<PlayerRuntime> players);
    void showBlock(std::size_t index, float taken, float left, std::span<PlayerRuntime> players);
    void updateTurbo(std::size_t index, int ticks, float seconds, std::span<PlayerRuntime> players,
                     const std::function<void(int, std::size_t)>& help);
    void updateProjectiles(float seconds, std::span<PlayerRuntime> players, const Targets& targets);
    void updateStrikes(float seconds, std::span<PlayerRuntime> players, const Targets& targets);
    void updateShields(float seconds, std::span<PlayerRuntime> players, const Targets& targets);
    void cry(std::size_t index, std::string_view which, std::span<PlayerRuntime> players);
    const MoveStrikes& strikes() const { return m_strikes; }
    std::size_t shieldCount() const { return m_shields.size(); }

private:
    static ItemArchive* moveEffectsOf(std::size_t index, std::span<PlayerRuntime> players);
    float ownDamageOf(std::size_t index, std::span<PlayerRuntime> players) const;
    void fireStrike(std::size_t index, int strikeIndex, std::span<PlayerRuntime> players);
    static constexpr int kEnemyTargetBase = 1000;
    static constexpr int kGeneratorTargetBase = 2000;
    static constexpr int kCritterTargetBase = 3000;
    static constexpr int kBossTargetBase = 4000;
    static constexpr int kSafeRockTargetBase = 5000;

    std::optional<Resources> m_resources;
    MoveStrikes m_strikes;
    /** The effect that goes along with a strike that flies. */
    struct StrikeEffect {
        unsigned int strike = 0;
        unsigned int effect = 0;
    };
    std::vector<StrikeEffect> m_strikeEffects;
    /** Whose strike a number is, and which of their class's, for what it shows on a hit. */
    struct StrikeSource {
        unsigned int strike = 0;
        std::size_t actor = 0;
        int row = -1;
    };
    std::vector<StrikeSource> m_strikeSources;
    /** A potion's magic ringing a character: it goes about with them and harms what it
     * touches, every so often, until it is spent. */
    struct PotionShield {
        std::size_t actor = 0;
        unsigned int effect = 0;
        float radius = 0.0f;
        float damage = 0.0f;
        float secondsLeft = 0.0f;
        float harmIn = 0.0f;
    };
    std::vector<PotionShield> m_shields;
};
} // namespace gdl::game
