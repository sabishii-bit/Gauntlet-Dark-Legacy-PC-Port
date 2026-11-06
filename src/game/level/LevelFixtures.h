#pragma once
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/LocalBinding.h"
#include "engine/core/Types.h"
#include "engine/io/LocalRoster.h"
#include "engine/io/LocalState.h"
#include "engine/io/NativeStateDigest.h"

#include "game/audio/LevelSoundscape.h"
#include "game/combat/DamageTypes.h"
#include "game/effects/EffectTrees.h"
#include "game/level/Breakables.h"
#include "game/level/Chests.h"
#include "game/level/CombatantObstacle.h"
#include "game/level/FallingScenery.h"
#include "game/level/FixtureHost.h"
#include "game/level/LevelWorld.h"
#include "game/level/LockedGates.h"
#include "game/level/MissileStop.h"
#include "game/level/Rubble.h"
#include "game/level/SafeRocks.h"
#include "game/level/StaticScenery.h"
#include "game/level/Traps.h"
#include "game/party/PlayerRuntime.h"
#include "game/players/MagicPerks.h"
namespace gdl::game {
/** Owns interactive scenery and its hazard chains. Scene services are borrowed while bound.
 * Reports go to the caller's FixtureHost at once, so health, help and opponent damage keep
 * their order. No player span or host is retained. Clear before world/archive/device
 * teardown. */
class LevelFixtures : public LocalState<LevelFixtures> {
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
              const CameraFrame* camera = nullptr, f32 presentationAlpha = -1.0f) const;
    /** Trap flames composite after deferred scenery, with solid-world depth testing. */
    void drawEffects(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                     const CameraFrame* camera = nullptr, f32 presentationAlpha = -1.0f) const;
    void capturePresentation();
    void update(s32 ticks, f32 seconds, std::span<PlayerRuntime> players, FixtureHost& host);
    /** Carry fixtures and container children without advancing gameplay during a camera cut. */
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
               std::vector<s32>& reached, FixtureHost& host);
    void strikeWall(usize index, f32 power, u32 flags = 0);
    /** A player's shot or blow of `radius` at `position` (a missile in flight, a strike's
     * ring, a swing, a potion's wave) bringing down the level's SHOOTFALL scenery
     * (fn_8005EE18), which then sounds the realm's break. */
    void shootScenery(const Vec3& position, f32 radius);
    void strikeBarrel(usize barrel, f32 power, s32 byPlayer, std::span<PlayerRuntime> players,
                      FixtureHost& host);
    /** What a broken barrel held comes out: its enemy when one is let out, else one pickup of
     * the record and count; when something came out, the hitter is taught that barrels hold
     * things. */
    void releaseBarrelContents(const BreakableStrike& struck, s32 byPlayer,
                               std::span<PlayerRuntime> players, FixtureHost& host);
    /** An explosion: a ring over its effect's `seconds` (StartExplosion, ProcessEffects mode
     * 1) growing from a third of `radius` to the whole as its harm falls from one and a half
     * times `damage` (a third of the way in) to nothing two thirds through, reaching each
     * thing once as it gets to it. */
    void blast(const Vec3& position, f32 radius, f32 damage, std::span<PlayerRuntime> players,
               FixtureHost& host, f32 seconds = kExplosionSeconds);
    void settleBlasts(std::span<PlayerRuntime> players, FixtureHost& host);
    /** Applies an already-expanded blast to containers and breakable scenery only.
     * The source owns `reached` for the blast lifetime, preventing repeated damage. */
    void blastScenery(const Vec3& position, f32 radius, f32 damage, u32 flags,
                      std::span<PlayerRuntime> players, FixtureHost& host,
                      std::vector<s32>& reached);
    /** WorldExplosion: realm-specific art and damage, at an animated or shot world object. */
    void worldExplosion(const Vec3& position, std::span<PlayerRuntime> players, FixtureHost& host);
    /** Gas reaching `radius` about a point with `damage` spoils the food lying there
     * (fn_8005C1DC's DMG_POISONGAS: over two), telling the party once it has. */
    void spoilFood(const Vec3& position, f32 radius, f32 damage,
                   std::span<const PlayerRuntime> players, FixtureHost& host);
    /** Applies one already-expanded blast step to pickups, without advancing its clock or
     * hurting characters again. Explosion damage shortens the item radius by 1.5. */
    void blastPickups(const Vec3& position, f32 radius, f32 damage, u32 flags,
                      std::span<const PlayerRuntime> players, FixtureHost& host);
    /** Grows the blasts under way by `seconds` (the update does, after the fixtures). */
    void advanceBlasts(f32 seconds, std::span<PlayerRuntime> players, FixtureHost& host);
    Chests& chests() { return m_chests; }
    const Chests& chests() const { return m_chests; }
    /** The developer console's hand on a gate: open without a key, sounding as it swings, or
     * shut again. */
    bool unlockGate(usize index);
    bool lockGate(usize index);
    /** The developer console's hand on a chest: wanting a key, or opening without one. */
    bool setChestLocked(usize index, bool locked) { return m_chests.setLocked(index, locked); }
    LockedGates& gates() { return m_gates; }
    const LockedGates& gates() const { return m_gates; }
    const Traps& traps() const { return m_traps; }
    const Breakables& barrels() const { return m_barrels; }
    const SafeRocks& safeRocks() const { return m_safeRocks; }
    SafeRocks& safeRocks() { return m_safeRocks; }
    const Rubble& rubble() const { return m_rubble; }
    const StaticScenery& scenery() const { return m_scenery; }
    static constexpr f32 kItemBlastInset = 1.5f; ///< DMG_EXPLODE shortens the item query
    static constexpr f32 kItemBlastPower = 5.0f; ///< the least that breaks an item apart
    static constexpr f32 kBarrelWarning = 9.0f;  ///< a player this near a spent barrel is told
    /** A blast yet to be felt: one barrel's sets off the next, in turn. */
    struct Blast {
        Vec3 position{0.0f, 0.0f, 0.0f};
        f32 radius = 0.0f;
        f32 damage = 0.0f;
        f32 seconds = kExplosionSeconds;
        f32 elapsed = 0.0f;
        u32 flags = damage::kFire | damage::kExplode |
                    damage::kKnockDown; ///< world effects supply their own
        bool started = false;
        bool done = false;
        std::vector<usize> players; ///< party indices it has reached
        std::vector<s32> reached;   ///< the fixtures it has reached
        std::vector<s32> opponents; ///< the opponents it has, by the scene's ids
        std::vector<std::pair<usize, f32>> playerReady; ///< poison repeats after 0.5s
        std::vector<std::pair<s32, f32>> opponentReady;
        bool operator==(const Blast&) const = default;
        template <class Visitor>
        friend void visitNativeState(Visitor& visitor, const Blast& value) {
            visitor.template fields<13>(value);
        }
    };
    /** The blasts under way, in the order they were set off, with what each has reached. */
    std::span<const Blast> blasts() const { return m_sim.blasts; }
    bool registerNativeResources(NativeStateResources& resources) const;

private:
    /** Answers a chest event: a key spent and the opening heard, its enemy let out before
     * any pickup is placed in it (none when the pickup's art is missing), gold taken, or the
     * lessons of a refusal. */
    void dispatchChestEvent(const ChestEvent& /*event*/, std::span<PlayerRuntime> /*players*/,
                            FixtureHost& /*host*/);
    /** Answers a gate event: a key spent and the gate heard as it opens, or the key lesson. */
    void dispatchGateEvent(const GateEvent& /*event*/, std::span<PlayerRuntime> /*players*/,
                           FixtureHost& /*host*/);
    static Vec3 observePlayer(PlayerRuntime& player, const Vec3& position);
    static void advanceHitSounds(std::span<PlayerRuntime> players, s32 ticks);
    static void advanceCloudGaps(std::span<PlayerRuntime> players, f32 seconds);
    void syncChestContents();
    void updateClouds(f32 seconds, std::span<PlayerRuntime> players, FixtureHost& host);
    /** A trapped chest goes up: its burst, its bang and a blast of fifty at the trap scale;
     * whoever opened it, or else the nearest standing, is told chests may do this. */
    void detonateChest(usize chest, std::optional<usize> opener, std::span<PlayerRuntime> players,
                       FixtureHost& host);
    /** An explosion's work on the chests and the shootable triggers within it. */
    void blastFixtures(const Vec3& position, f32 reach, f32 damage, FixtureHost& host,
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
        bool operator==(const GasCloud&) const = default;
        template <class Visitor>
        friend void visitNativeState(Visitor& visitor, const GasCloud& value) {
            visitor.template fields<4>(value);
        }
    };
    /** One step of a blast's ring, as it stands `elapsed` into its life. */
    void feel(Blast& ring, std::span<PlayerRuntime> players, FixtureHost& host);

    struct Simulation {
        std::vector<GasCloud> clouds;
        std::vector<Blast> blasts;
        std::vector<usize> doomedChests;
        bool operator==(const Simulation&) const = default;
    };
    Simulation m_sim;

    Chests m_chests;
    LockedGates m_gates;
    Traps m_traps;
    Breakables m_barrels;
    SafeRocks m_safeRocks;
    StaticScenery m_scenery;
    Rubble m_rubble;
    LocalBinding m_localBinding;

    friend class LocalState<LevelFixtures>;
    static constexpr LocalTraits kLocalTraits{.refusalInvalidates = true,
                                              .gatesOnBound = false,
                                              .gatesOnValues = false,
                                              .notifiesRestore = false};
    using LocalValues = Simulation;
    /** A snapshot's slot for each fixture kind. */
    struct LocalChildren {
        Chests::LocalSnapshot chests;
        LockedGates::LocalSnapshot gates;
        Traps::LocalSnapshot traps;
        Breakables::LocalSnapshot barrels;
        SafeRocks::LocalSnapshot safeRocks;
        StaticScenery::LocalSnapshot scenery;
        Rubble::LocalSnapshot rubble;
    };
    /** Its own values, then each fixture kind's, in digest order. The world, effects and audio
     * it borrows and the players it is handed are other owners, which the scene saves,
     * compares and hashes; so a restore takes back values only, never stopping a cloud's
     * effect or reporting a contact. */
    static constexpr auto localParts() {
        return local::parts(local::own(),
                            local::child(&LevelFixtures::m_chests, &LocalChildren::chests),
                            local::child(&LevelFixtures::m_gates, &LocalChildren::gates),
                            local::child(&LevelFixtures::m_traps, &LocalChildren::traps),
                            local::child(&LevelFixtures::m_barrels, &LocalChildren::barrels),
                            local::child(&LevelFixtures::m_safeRocks, &LocalChildren::safeRocks),
                            local::child(&LevelFixtures::m_scenery, &LocalChildren::scenery),
                            local::child(&LevelFixtures::m_rubble, &LocalChildren::rubble));
    }
    const LocalValues& localValues() const { return m_sim; }
    LocalValues& localValues() { return m_sim; }
    /** Writes the schema, what the fixtures were bound to and `state`. */
    bool digestLocalValues(NativeStateDigest& digest, const LocalValues& state) const;
};
} // namespace gdl::game
