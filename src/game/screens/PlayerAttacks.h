#pragma once
#include <functional>
#include <optional>
#include <span>

#include "engine/core/Types.h"
#include "engine/world/AmbientDimmer.h"

#include "game/players/ItemAttack.h"
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
    void ramBarrels(usize index, std::span<PlayerRuntime> players, const Targets& targets);
    void shieldPotion(usize index, std::span<PlayerRuntime> players);
    void usePotion(usize index, std::span<PlayerRuntime> players);
    void useItemAttack(usize index, std::span<PlayerRuntime> players);
    void showBlock(usize index, f32 taken, f32 left, std::span<PlayerRuntime> players);
    void updateTurbo(usize index, s32 ticks, f32 seconds, std::span<PlayerRuntime> players,
                     const std::function<void(s32, usize)>& help);
    void updateProjectiles(f32 seconds, std::span<PlayerRuntime> players, const Targets& targets);
    void updateStrikes(f32 seconds, std::span<PlayerRuntime> players, const Targets& targets);
    void updateShields(f32 seconds, std::span<PlayerRuntime> players, const Targets& targets);
    void cry(usize index, std::string_view which, std::span<PlayerRuntime> players);
    std::optional<Vec3> aim(const PlayerActor& actor, const Vec3& facing,
                            const Targets& targets) const;
    PlayerDeed attackDeed(const PlayerActor& actor, bool strong, const Targets& targets) const;
    void melee(usize index, std::span<PlayerRuntime> players, const Targets& targets);
    const MoveStrikes& strikes() const { return m_strikes; }
    usize shieldCount() const { return m_shields.size(); }

private:
    void beginPotion(const MissileImpact& impact);
    void updatePotions(f32 seconds, std::span<PlayerRuntime> players, const Targets& targets);
    void updateItems(f32 seconds, std::span<PlayerRuntime> players, const Targets& targets);
    static void strikeTarget(const MissileTarget& target, f32 damage, u32 flags,
                             const PlayerActor& owner, std::span<PlayerRuntime> players,
                             const Targets& targets);
    struct ItemArea {
        ItemAttack attack;
        usize actor = 0;
        u32 effect = 0;
        f32 elapsed = 0;
        f32 lifetime = 1;
        std::vector<s32> hit;
    };
    std::vector<ItemArea> m_items;
    struct PotionBurst {
        MissileImpact impact;
        f32 elapsed = 0;
        f32 duration = 1;
        std::vector<s32> hit;
    };
    std::vector<PotionBurst> m_potions;
    static std::vector<MissileTarget> projectileTargets(const Targets& targets);
    static ItemArchive* moveEffectsOf(usize index, std::span<PlayerRuntime> players);
    f32 ownDamageOf(usize index, std::span<PlayerRuntime> players) const;
    void fireStrike(usize index, s32 strikeIndex, std::span<PlayerRuntime> players);
    static constexpr s32 kEnemyTargetBase = 1000;
    static constexpr s32 kGeneratorTargetBase = 2000;
    static constexpr s32 kCritterTargetBase = 3000;
    static constexpr s32 kBossTargetBase = 4000;
    static constexpr s32 kSafeRockTargetBase = 5000;

    std::optional<Resources> m_resources;
    MoveStrikes m_strikes;
    /** The effect that goes along with a strike that flies. */
    struct StrikeEffect {
        u32 strike = 0;
        u32 effect = 0;
    };
    std::vector<StrikeEffect> m_strikeEffects;
    /** Whose strike a number is, and which of their class's, for what it shows on a hit. */
    struct StrikeSource {
        u32 strike = 0;
        usize actor = 0;
        s32 row = -1;
        struct Contact {
            s32 target = -1;
            f32 remaining = 0;
        };
        std::vector<Contact> contacts;
    };
    std::vector<StrikeSource> m_strikeSources;
    /** A potion's magic ringing a character: it goes about with them and harms what it
     * touches, every so often, until it is spent. */
    struct PotionShield {
        usize actor = 0;
        u32 flags = 0;
        u32 effect = 0;
        f32 radius = 0.0f;
        f32 damage = 0.0f;
        f32 secondsLeft = 0.0f;
        f32 harmIn = 0.0f;
    };
    std::vector<PotionShield> m_shields;
};
} // namespace gdl::game
