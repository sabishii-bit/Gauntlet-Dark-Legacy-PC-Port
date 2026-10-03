#pragma once
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/Types.h"

#include "game/enemies/Combatant.h"
#include "game/enemies/EnemyMissiles.h"
#include "game/players/MagicPerks.h"
#include "game/screens/PlayerHealth.h"
#include "game/world/Breakables.h"
#include "game/world/Chests.h"
#include "game/world/EffectTrees.h"
#include "game/world/FallingScenery.h"
#include "game/world/LevelSoundscape.h"
#include "game/world/LevelWorld.h"
#include "game/world/LockedGates.h"
#include "game/world/Rubble.h"
#include "game/world/SafeRocks.h"
#include "game/world/Traps.h"
namespace gdl::game {
/** Owns interactive scenery and its hazard chains. Scene services are borrowed while bound.
 * Events are synchronous so health, help and opponent damage retain their original order.
 * No player span or callback is retained. Clear before world/archive/device teardown. */
class LevelFixtures {
public:
    static constexpr f32 kBlastRadius = 12.0f;
    static constexpr f32 kExplosionSeconds = 1.0f; ///< EXPLOSION's thirty frames
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
        std::function<bool(s32, usize)> help; ///< whether the message went up
        std::function<void(s32, std::string_view)> card;
        /** A blast's ring reaching `radius` about a point with `damage`: the swarm, the
         * great ones, the boss and the generators within it and not yet in the blast's
         * `reached` (ids of the caller's), which a blow over two joins. */
        std::function<void(const Vec3&, f32, f32, std::vector<s32>&, u32)> opponents;
        std::function<bool(s32, const Vec3&, s32)> releaseEnemy;
        std::function<void(s32, const Vec3&)> shatterPotion;
    };
    void bind(const Resources& resources);
    void clear();
    void setPlayerCount(s32 count);
    std::vector<Obstacle> obstacles() const;
    /** What stops the swarm's missiles (fn_8005ED44's candidates): the solid obstacles, the
     * standing safe rocks as the rocks they are, the bottles lying about, the triggers that
     * are shot and the tent walls while raised. */
    std::vector<MissileStop> missileStops() const;
    /** What stops a thrown weapon unharmed (SfxSkipItem): the chests, the shut gates and the
     * tent walls while raised. */
    std::vector<Obstacle> inertStops() const;
    /** What stands where the great ones walk (fn_8005D5C8): the chests, which golems and
     * gargoyles walk through, the standing barrels, which they break, and the gates and
     * standing safe rocks, which stop them. */
    std::vector<CombatantObstacle> critterObstacles() const;
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const CameraFrame* camera = nullptr) const;
    /** Trap flames composite after deferred scenery, with solid-world depth testing. */
    void drawEffects(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                     const CameraFrame* camera = nullptr) const;
    void update(s32 ticks, f32 seconds, std::span<PlayerRuntime> players, const Events& events);
    /** Carry containers and their children without advancing gameplay during a camera cut. */
    void syncFloors();
    void strikeSafeRock(usize index, f32 power);
    /** Magic on a shut chest holding Death (fn_8005C1DC): he becomes the level's apple, with
     * his dying cry, and the chest rocks. False for any other chest. */
    bool enchantChest(usize index, f32 power);
    /** A wave of potion magic of a caster (party index `caster`) with a class perk, reaching
     * `radius` about `position` (fn_8005BA1C): junk into treasure, spoiled food cleansed, in
     * the open or in a shut chest or a barrel; traps stopped or disarmed; secret walls shown
     * up or brought down. Each change shows its family's LEVELUP tree and teaches the caster
     * its lesson once. `reached` keeps what the wave has touched so each is touched once. */
    void bless(const Vec3& position, f32 radius, MagicPerk perk, usize caster,
               std::vector<s32>& reached, const Events& events);
    void strikeWall(usize index, f32 power, u32 flags = 0);
    /** A player's shot or blow of `radius` at `position` (a missile in flight, a strike's
     * ring, a swing, a potion's wave) bringing down the level's SHOOTFALL scenery
     * (fn_8005EE18), which then sounds the realm's break. */
    void shootScenery(const Vec3& position, f32 radius);
    void strikeBarrel(usize barrel, f32 power, s32 byPlayer, std::span<PlayerRuntime> players,
                      const Events& events);
    /** An explosion: a ring over its effect's `seconds` (StartExplosion, ProcessEffects mode
     * 1) growing from a third of `radius` to the whole as its harm falls from one and a half
     * times `damage` (a third of the way in) to nothing two thirds through, reaching each
     * thing once as it gets to it. */
    void blast(const Vec3& position, f32 radius, f32 damage, std::span<PlayerRuntime> players,
               const Events& events, f32 seconds = kExplosionSeconds);
    void settleBlasts(std::span<PlayerRuntime> players, const Events& events);
    /** WorldExplosion: realm-specific art and damage, at an animated or shot world object. */
    void worldExplosion(const Vec3& position, std::span<PlayerRuntime> players,
                        const Events& events);
    /** Gas reaching `radius` about a point with `damage` spoils the food lying there
     * (fn_8005C1DC's DMG_POISONGAS: over two), telling the party once it has. */
    void spoilFood(const Vec3& position, f32 radius, f32 damage,
                   std::span<const PlayerRuntime> players, const Events& events);
    /** Applies one already-expanded blast step to pickups, without advancing its clock or
     * hurting characters again. Explosion damage shortens the item radius by 1.5. */
    void blastPickups(const Vec3& position, f32 radius, f32 damage, u32 flags,
                      std::span<const PlayerRuntime> players, const Events& events);
    /** Grows the blasts under way by `seconds` (the update does, after the fixtures). */
    void advanceBlasts(f32 seconds, std::span<PlayerRuntime> players, const Events& events);
    Chests& chests() { return m_chests; }
    const Chests& chests() const { return m_chests; }
    LockedGates& gates() { return m_gates; }
    const LockedGates& gates() const { return m_gates; }
    const Traps& traps() const { return m_traps; }
    const Breakables& barrels() const { return m_barrels; }
    const SafeRocks& safeRocks() const { return m_safeRocks; }
    SafeRocks& safeRocks() { return m_safeRocks; }
    const Rubble& rubble() const { return m_rubble; }
    static constexpr f32 kItemBlastInset = 1.5f; ///< DMG_EXPLODE shortens the item query
    static constexpr f32 kItemBlastPower = 5.0f; ///< the least that breaks an item apart
    static constexpr f32 kBarrelWarning = 9.0f;  ///< a player this near a spent barrel is told

private:
    void syncChestContents();
    void updateClouds(f32 seconds, std::span<PlayerRuntime> players, const Events& events);
    /** A trapped chest goes up: its burst, its bang and a blast of fifty at the trap scale;
     * whoever opened it, or else the nearest standing, is told chests may do this. */
    void detonateChest(usize chest, std::optional<usize> opener, std::span<PlayerRuntime> players,
                       const Events& events);
    /** An explosion's work on the chests and the shootable triggers within it. */
    void blastFixtures(const Vec3& position, f32 reach, f32 damage, const Events& events,
                       std::vector<s32>& reached, bool destroysContainers);
    /** Leaves `object` as rubble placed by `transform`, from the level's items. */
    void leaveRubble(std::string_view object, const Mat4& transform);
    static std::optional<usize> nearestStanding(std::span<const PlayerRuntime> players,
                                                const Vec3& position, f32 reach);
    void playGateSound(s32 subtype);
    void playRealmSound(std::string_view stem);
    /** Sounds the scenery that gave way (fn_8009D8CC, fn_8009D91C). */
    void playFallingCues(std::span<const FallingCue> cues);
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
        f32 seconds = kExplosionSeconds;
        f32 elapsed = 0.0f;
        u32 flags = 0x421; ///< fire + explosion + knockdown; world effects supply their own
        bool started = false;
        bool done = false;
        std::vector<usize> players; ///< party indices it has reached
        std::vector<s32> reached;   ///< the fixtures it has reached
        std::vector<s32> opponents; ///< the opponents it has, by the scene's ids
        std::vector<std::pair<usize, f32>> playerReady; ///< poison repeats after 0.5s
        std::vector<std::pair<s32, f32>> opponentReady;
    };
    /** One step of a blast's ring, as it stands `elapsed` into its life. */
    void feel(Blast& ring, std::span<PlayerRuntime> players, const Events& events);
    std::vector<Blast> m_blasts;

    Chests m_chests;
    LockedGates m_gates;
    Traps m_traps;
    Breakables m_barrels;
    SafeRocks m_safeRocks;
    Rubble m_rubble;
    std::vector<usize> m_doomedChests; ///< trapped chests a blast set off, to go up next update
};
} // namespace gdl::game
