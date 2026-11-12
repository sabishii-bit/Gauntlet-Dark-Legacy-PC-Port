#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/bosses/BossDefinition.h" // IWYU pragma: export
#include "game/bosses/LegendItems.h"
#include "game/bosses/LegendProfile.h"
#include "game/critters/Combatant.h"

namespace gdl::game {

class LevelTriggers;
class NativeStateResources;

/** A step of a legend item's rite the game shows: which, and whose item it is. */
struct LegendEvent {
    LegendCue cue = LegendCue::Brandished;
    s32 player = -1;
    s32 realm = 0;
    bool operator==(const LegendEvent&) const = default;
    friend void visitNativeState(NativeStateDigest& digest, const LegendEvent& value) {
        digest.fields<3>(value);
    }
};

/** The boss a realm keeps: which, and how it stands. */
struct BossView {
    s32 kind = -1;    ///< the original's kind, 34 the dragon to 44 the garm
    std::string name; ///< "LICH"
    f32 health = 0.0f;
    f32 maxHealth = 1.0f;
    bool awake = false;
    /** The encounter has begun (retail's gBossActive): from the wake, or from the arming of a
     * boss that waits for its floor to move, at least two seconds before it wakes. The fight's
     * camera frames it from then on, and a boss level's powerup timers run three times as fast
     * from the update it begins in. */
    bool engaged = false;
    /** Its death has played (retail's gBossDead): from the update its DEATH is done and its
     * hold runs, and for good once a boss that woke is gone. A boss level's powerup timers
     * stop then. */
    bool deathPlayed = false;
    bool alive = false;
    f32 fraction() const { return maxHealth > 0.0f ? std::max(health, 0.0f) / maxHealth : 0.0f; }
};

/**
 * One realm encounter: owns a single combatant and its assets, waking and legendary-item
 * progression. Shared move execution belongs to Combatant; population limits and ordinary
 * creature spawning do not apply to the boss.
 */
class Bosses {
public:
    Bosses() = default;
    Bosses(const Bosses&) = delete;
    Bosses& operator=(const Bosses&) = delete;
    Bosses(Bosses&&) = delete;
    Bosses& operator=(Bosses&&) = delete;
    ~Bosses() = default;

    void open(RenderDevice& device, const std::filesystem::path& unpackedRoot,
              const WorldCollision* collision, const EnemyScales& scales, char realm,
              std::span<TextureSet* const> textureLenders = {},
              const LegendProfile* legendProfile = nullptr);
    void close();

    /** Stands the boss of `kind` (`bossNameOf` it) at `position` facing `yaw`, asleep until
     * the party comes within `wakeDistance` (its table's threshold when nought). */
    bool spawn(s32 kind, const Vec3& position, f32 yaw, f32 wakeDistance = 0.0f);
    /** Load render assets without creating a fighter or beginning an encounter. */
    CombatantAssets* preload(s32 kind);
    /** Installs an imported encounter's complete native archive before spawning.
     * Rejects replacement of live/cached borrowers; no fallback data is read. */
    CombatantAssets* preload(s32 kind, std::unique_ptr<CombatantAssets> assets);

    /** The realm of the legend item that weakens this boss, or nought. */
    s32 legendRealm() const { return legendRealmOf(m_sim.kind); }
    /** Begins the rite of the boss's legend item, carried by `player`; false when the boss
     * has none, or it is already begun. */
    bool bringLegend(s32 player);
    /** The axe/lamp/scimitar/javelin act on impact; Savior acts on cast release.
     * Repeated notifications are ignored. Appearance is supplied separately at draw time. */
    void landLegend();
    const LegendRite& legend() const { return m_sim.rite; }
    /** Actual animated attachment targeted by this encounter's legendary item. */
    std::optional<Mat4> legendTargetTransform() const;

    /** Asleep, a boss arms its wake deadline on its first update with `activationReady` (an
     * imported level's signal, sampled before the update) and never without it. Given the
     * base game's `triggers`, a boss whose type waits for its floor to move
     * (CritterData::kSupportActivated) arms only on such an update on which one of them moves
     * the floor its probe stands it on. */
    void update(s32 ticks, f32 seconds, std::span<const EnemyView> players,
                bool activationReady = true, const LevelTriggers* triggers = nullptr);
    /** Runs through the held entrance too. It never arms a boss that waits for the base
     * game's triggers, which do not move during the entrance. Arming is sticky. */
    void arm(f32 seconds, bool activationReady = true, const LevelTriggers* triggers = nullptr);
    void setArenaAnchors(std::span<const Mat4> anchors) { m_fighter.setArenaAnchors(anchors); }
    void setArenaTargets(std::span<const CombatArenaTarget> targets) {
        m_fighter.setArenaTargets(targets);
    }
    bool raisesArenaRocks() const { return m_fighter.raisesArenaRocks(); }
    std::vector<CombatArenaActivation> takeArenaActivations() {
        return m_fighter.takeArenaActivations();
    }
    std::vector<CombatBlow> takeBlows();
    std::vector<CombatGrab> takeGrabs() { return m_fighter.takeGrabs(); }
    std::vector<CombatLoss> takeLosses();
    /** One notification after the death animation and hold finish, not on the lethal hit. */
    std::optional<Vec3> takeDefeat();
    std::vector<CombatCue> takeCues();
    std::vector<CombatShot> takeShots() { return m_fighter.takeShots(); }
    /** What its death threw out: the coins it spews. */
    std::vector<CombatSpew> takeSpews() { return m_fighter.takeSpews(); }
    std::vector<LegendEvent> takeLegendEvents();
    f32 hurt(const EnemyHit& hit, s32 partId = kTargetId);
    void damagedPlayer(s32 player, f32 amount, s32 partId = kTargetId);
    /** Wakes it as the party coming within its threshold would. */
    void wake();
    bool frozen() const;
    bool blinded() const;
    bool curbed() const;

    std::vector<MissileTarget> targets(bool solidOnly = false) const;
    std::optional<s32> struckBy(const Vec3& from, const Vec3& to, f32 radius) const;
    bool within(const Vec3& centre, f32 radius) const;
    bool reachedBy(const Vec3& centre, f32 radius, f32 arc, const Vec3& facing) const;
    void drawShadow(RenderDevice& device, const Mat4& clip, const Vec3& eye,
                    const WorldLighting& lighting, f32 presentationAlpha = -1) const {
        m_fighter.drawShadow(device, clip, eye, lighting, presentationAlpha);
    }
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const Texture* frozenTexture = nullptr, f32 presentationAlpha = -1) const;
    /** The white skin a hard hit flashes over the body (`Combatant::flashing`); borrowed
     * until close(), none for no flash. */
    void setHitFlash(const Texture* texture) { m_hitFlash = texture; }

    bool present() const { return m_sim.id.has_value(); }
    const Combatant& fighter() const { return m_fighter; }
    std::vector<CombatantAssets*> resources();
    std::vector<const CombatantAssets*> resources() const;
    BossView view() const;
    /** How its meter is laid out, or null without a boss. */
    const HealthMeterDefinition* meter() const;
    /** All enabled HUD meters, including defeated heads until the encounter ends. */
    std::vector<HealthMeterReading> healthMeters() const;
    /** Its archive, whose textures the meter is drawn from; null without a boss. */
    ItemArchive* archive();
    const Vec3* position() const;
    std::optional<Mat4> nodeTransform(std::string_view node) const;
    std::optional<Mat4> rootTransform() const;
    /** The way it faces, and how big it stands; nought without a boss. */
    f32 facing() const;
    f32 radius() const;
    /** How high its body's centre stands, which the fight's camera keeps in view. */
    f32 height() const;
    /** The tracking camera anchor includes the model root and vertical drift. */
    Vec3 cameraOffset() const;
    /** The root its type was first stood at, kept for good: the floor CritterInitGeo's probe
     * finds under the placement, else the placement's own height, raised by the type's
     * floorOffset (+0x418, 0x8003e60c-0x8003e6fc). The fight's camera keeps it as its base; none
     * until a boss has stood this level. */
    const std::optional<Vec3>& spawnRoot() const { return m_sim.spawnRoot; }
    /** Where BossDeath leaves the fallen boss's key: its spawn root plus its type's key offset,
     * in world axes, wherever the boss went meanwhile (0x8001b908-0x8001b960); none until a boss
     * has stood this level. */
    std::optional<Vec3> keySpot() const;
    std::string_view moveName() const;
    /** The id targets and sweeps name the boss by. */
    static constexpr s32 kTargetId = 0;
    static constexpr f32 kWakeArmSeconds = 2.0f; ///< asleep at least this long after placing

private:
    /** Moves the rite on, told the type of the move the fighter played as the update began. */
    void stageLegend(s32 ticks, s32 movedFrom);
    void strikeWithLegend();
    /** Whether its type arms only once its floor moves (TYPE flag 0x80). */
    bool supportActivated() const;
    /** Whether this update may arm its wake deadline, `support` being the floor its probe
     * found (none in the entrance, which does not probe). */
    bool deadlineMayStart(bool activationReady, const LevelTriggers* triggers,
                          const std::optional<FloorHit>& support) const;
    /** Arms the wake deadline if it may, then counts this update's time toward it once armed:
     * the deadline is set as the update ends and first tested in the next. */
    void advanceWakeClock(f32 seconds, bool mayStart);

    Combatant m_fighter;
    // Effects and projectiles can outlive their boss. Retain borrowed assets until close().
    std::vector<std::unique_ptr<CombatantAssets>> m_assetCache;
    RenderDevice* m_device = nullptr;
    std::filesystem::path m_root;
    std::vector<TextureSet*> m_textureLenders; ///< stage owners outlive this encounter
    const WorldCollision* m_collision = nullptr;
    EnemyScales m_scales;
    char m_realm = 'G';
    // Copy immutable configuration: no borrowed profile can dangle or change
    // under an in-memory rewind. Reopening invalidates the snapshot epoch.
    std::optional<LegendProfile> m_legendProfile;
    struct Simulation {
        std::vector<CombatantAssets*> assets;
        std::optional<s32> id;
        std::optional<Vec3> spawnRoot;
        std::optional<Vec3> defeat;
        Vec3 keyOffset{0}; ///< its type's, retained through removal for the key
        s32 kind = -1;
        std::string name;
        bool awake = false;
        f32 wakeDistance = 0.0f;
        /** Elapsed since it armed: at placement, or once its floor first moved; nought
         * until then. */
        f32 wakeClock = 0.0f;
        LegendRite rite;
        bool roarAsked = false;
        bool legendStruck = false;
        std::vector<LegendEvent> legendEvents;
    };

public:
    class LocalSnapshot {
        friend class Bosses;
        Simulation m_state;
        Combatant::LocalSnapshot m_fighter;
        std::vector<TextureAnimator::Clock> m_clocks;
        std::vector<std::weak_ptr<const u8>> m_assets;
        std::weak_ptr<const u8> m_owner;
        u64 m_epoch = 0;
    };
    bool saveLocal(LocalSnapshot& snapshot) const;
    bool canRestoreLocal(const LocalSnapshot& snapshot) const;
    /** Same-owner native encounter, fighter/children, legend latches and rewards. */
    bool equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const;
    bool restoreLocal(const LocalSnapshot& snapshot);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;
    bool registerNativeResources(NativeStateResources& resources,
                                 std::string_view stablePrefix) const;

private:
    static void
    appendSimulationDigest(NativeStateDigest& digest, const Simulation& state,
                           std::optional<std::span<const TextureAnimator::Clock>> savedClocks = {});
    Simulation m_sim;
    std::shared_ptr<const u8> m_localOwner = std::make_shared<const u8>(0);
    u64 m_localEpoch = 0;
    const Texture* m_hitFlash = nullptr; ///< borrowed from the level
};

} // namespace gdl::game
