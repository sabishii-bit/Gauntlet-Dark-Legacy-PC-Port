#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/TreeModel.h"
#include "engine/world/WorldCamera.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/combat/DamageTypes.h"
#include "game/combat/MissileTarget.h"
#include "game/config/MultiplayerMode.h"
#include "game/effects/EffectTrees.h"
#include "game/effects/MissileStreak.h"

namespace gdl::game {

/** How a class's thrown weapon flies, from the original's tables. */
struct MissileSpec {
    std::string_view model;   ///< the throw trees are named `<model>_THROW<tier>`
    std::string_view tiers;   ///< the tier for each ten levels; '0' is the costume's own
    f32 radius = 1.0f;        ///< what walls stop
    f32 spin = 0.0f;          ///< radians a second it tumbles forwards
    f32 weight = 8.0f;        ///< how hard it falls, units a second squared
    bool staysInHand = false; ///< a staff or a bow is not what flies
    std::string_view impactTree = "SPARKS"; ///< the weapon's world-contact effect

    /** The spec of a class (the unlockable classes fly like the class they shadow). */
    static const MissileSpec& of(s32 classIndex);
    /** The name of the class's throw tree at `level`, and whether the costume's own archive
     * holds it (else the costume colour's effects archive does). */
    static std::string treeName(s32 classIndex, s32 level, bool* inCostume = nullptr);
    /** Whether a class throws by its magic rather than its strength. */
    static bool byMagic(s32 classIndex);
    /** How a thrown potion flies. */
    static const MissileSpec& potion();
    static const MissileSpec& superShot();
};

/** Spread volleys sound only on the centre projectile. */
enum class MissileWallSound : u8 { Level, ThreeWay, FiveWay, Silent, Ricochet };

/** Immutable weapon-release provenance captured before later world updates can
 * interrupt its animation. Session tick/grant identity belongs to the caller. */
struct MissileRelease {
    s32 character = 0;
    u64 generation = 0;
    bool strong = false;
    u8 ordinal = 0;   ///< native fan order, before any projectile collides or disappears
    u32 emission = 0; ///< zero for ordinary throws, otherwise TurboMove's slot/counter identity
    bool operator==(const MissileRelease&) const = default;
    template <class D> friend void visitNativeState(D& digest, const MissileRelease& value) {
        digest.template fields<5>(value);
    }
};

/** Potion throws have their own action generation and never alias weapon volleys. */
struct MissilePotionRelease {
    u64 generation = 0;
    bool operator==(const MissilePotionRelease&) const = default;
    template <class D> friend void visitNativeState(D& digest, const MissilePotionRelease& value) {
        digest.template fields<1>(value);
    }
};

/** What sets a missile off. */
struct MissileLaunch {
    s32 owner = 0;
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 direction{0.0f, 0.0f, 1.0f}; ///< along the ground, unit length
    f32 speed = 20.0f;
    f32 reach = 15.0f; ///< how far off it comes back down to just under where it left
    const MissileSpec* spec = nullptr;
    const TreeModel* model = nullptr; ///< must outlive the missile
    ItemArchive* archive = nullptr;   ///< optional full tree playback; outlives clear()
    std::string_view tree;
    TextureSet* textureLender = nullptr; ///< the shooter's SFX textures, outlive clear()
    ItemArchive* riderArchive = nullptr; ///< an effect that rides the missile; outlives clear()
    std::string_view riderTree;
    std::optional<Vec3> velocity; ///< set, it flies off at this instead of being lobbed
    s32 potion = 0;               ///< the kind of potion it is, which bursts where it lands
    f32 potency = 0.0f;           ///< the magic power its burst goes off with
    f32 damage = 0.0f;            ///< what it does to what it hits
    f32 scale = 1.0f;             ///< how large it is drawn: a strong throw's is doubled
    MissileWallSound wallSound = MissileWallSound::Level;
    u32 flags = 0;
    MissileStreak streak;
    MultiplayerMode multiplayer = MultiplayerMode::Normal;
    f32 playerHitGap = 0.25f;  ///< hit-effect frames / 30, when authored
    bool breaksPotions = true; ///< companion shots pass through collectible bottles
    std::optional<MissileRelease> release;
    std::optional<MissilePotionRelease> potionRelease;
};

/** Where a missile was stopped. */
struct MissileImpact {
    Vec3 position{0.0f, 0.0f, 0.0f};
    s32 owner = 0;
    s32 potion = 0;
    f32 potency = 0.0f;
    f32 damage = 0.0f;
    s32 target = -1; ///< the id of the target it stopped against; none for a wall or the floor
    std::string_view effect = "SPARKS";
    MissileWallSound wallSound = MissileWallSound::Level;
    u32 flags = 0;
    s32 node = -1;
    s32 worldObject = -1; ///< struck collision owner, independent of a creature target
    bool liquid = false;  ///< water contact replaces ordinary impact audio
    s32 player = -1;      ///< input lane of a struck player, independent of world target ids
    Vec3 direction{0.0f};
    bool stun = false;
    f32 playerHitGap = 0.25f;
    template <class D> friend void visitNativeState(D& digest, const MissileImpact& value) {
        digest.template fields<16>(value);
    }
};

/** Player cylinders supplied separately from world targets, including missile reflection. */
struct MissilePlayer {
    s32 player = 0;
    MissileTarget body;
    bool reflective = false;
};

/**
 * The weapons the party has thrown, flying the way the original's do: along the thrower's
 * facing at the pace their strength (or magic) gives, lobbed so that gravity brings them
 * back to half a unit under their start at their reach, tumbling if their kind does, and
 * gone the moment a wall or floor stops them or their time runs out.
 */
class PlayerMissiles {
public:
    PlayerMissiles() = default;
    PlayerMissiles(const PlayerMissiles&) = delete;
    PlayerMissiles& operator=(const PlayerMissiles&) = delete;
    PlayerMissiles(PlayerMissiles&&) = delete;
    PlayerMissiles& operator=(PlayerMissiles&&) = delete;
    ~PlayerMissiles() = default;
    static constexpr u32 kPlainModifiers =
        damage::kRapidFire | damage::kThreeWayShot | damage::kFiveWayShot;
    static constexpr u32 kStrongThrowFlags = damage::kTurbo | damage::kKnockBack;
    static constexpr u32 kReplayModifiers =
        kPlainModifiers | kStrongThrowFlags | damage::kElementMask;
    /** Ordinary elemental or plain throws only; other weapon modes remain authoritative. */
    static bool replayModifiers(u32 flags);
    static constexpr f32 kSlowest = 20.0f; ///< units a second with no strength at all
    static constexpr f32 kFastest = 60.0f; ///< and at a stat of 1000
    static constexpr f32 kStatScale = 0.001f;
    static constexpr f32 kReach = 15.0f;           ///< of a tapped throw
    static constexpr f32 kReachPerSecond = 200.0f; ///< more for each second the attack is held
    static constexpr f32 kHoldDelay = 0.27f;       ///< before holding counts
    static constexpr f32 kHoldMost = 0.1f;         ///< and the most of it that does
    static constexpr f32 kDrop = 0.5f;             ///< under its start, where its reach lands it
    static constexpr f32 kMuzzle = 2.0f;           ///< ahead of the hand, where it appears
    static constexpr f32 kLifeSeconds = 3.0f;
    static constexpr f32 kLeastDamage = 5.0f; ///< of a missile, with no strength at all
    static constexpr f32 kMostDamage = 20.0f; ///< and at a stat of 1000

    /** Release-time inputs for the native weapon muzzle, spread and ballistic aim.
     * Contains no artwork, audio, inventory mutation or collision side effects. */
    struct WeaponThrow {
        s32 owner = 0;
        s32 character = 0;
        Vec3 position{0}; ///< feet; target-facing correction is measured from here
        Vec3 center{0};   ///< body follow point, before the authored hand offset
        Vec3 hand{0};
        Vec3 direction{0, 0, 1};
        f32 speed = kSlowest;
        f32 damage = 0;
        f32 attackSeconds = 0;
        f32 scale = 1;
        u32 flags = 0;
        s32 shots = 1;
        MultiplayerMode multiplayer = MultiplayerMode::Normal;
        std::optional<Vec3> target;
        bool fixedFacing = false; ///< cursor controls yaw; a target only adjusts pitch
    };

    /** Resumable flight of one ordinary weapon. Resource bindings and identity
     * remain in the local pool; impact/piercing state is deliberately excluded. */
    struct FlightCheckpoint {
        Vec3 position{0};
        Vec3 velocity{0};
        f32 tumble = 0;
        f32 age = 0;
        f32 scale = 1;
        u32 continuity = 1;
        bool valid() const;
        bool operator==(const FlightCheckpoint&) const = default;
        template <class D> friend void visitNativeState(D& digest, const FlightCheckpoint& value) {
            digest.template fields<6>(value);
        }
    };

    /** Cosmetic history for native particle nodes. An impact stops emission before
     * that tick's visual update; its final placement is carried by the effect. */
    struct ParticleFlight {
        FlightCheckpoint birth;
        f32 stepSeconds = 1.0f / 60;
        u32 activeSteps = 0;
        u32 totalSteps = 0;
        bool retiring = false;
        bool valid() const;
        bool operator==(const ParticleFlight&) const = default;
        template <class D> friend void visitNativeState(D& digest, const ParticleFlight& value) {
            digest.template fields<5>(value);
        }
    };

    struct VisualHistory {
        u64 instance = 0;
        s32 owner = 0;
        s32 character = 0;
        std::optional<MissileRelease> release;
        u32 effect = 0;
        u32 rider = 0;
        ParticleFlight flight;
        Mat4 placement{1};
        Vec3 direction{0};
        s32 potion = 0; ///< zero selects the ordinary class spec; 1..4 use MissileSpec::potion
        std::optional<MissilePotionRelease> potionRelease;
        bool operator==(const VisualHistory&) const = default;
        template <class D> friend void visitNativeState(D& digest, const VisualHistory& value) {
            digest.template fields<11>(value);
        }
    };

    /** One weapon in flight. */
    struct Missile {
        u64 instance = 0;
        u32 continuity = 1;
        s32 owner = 0;
        Vec3 position{0.0f, 0.0f, 0.0f};
        Vec3 velocity{0.0f, 0.0f, 0.0f};
        f32 tumble = 0.0f; ///< how far it has turned over
        f32 age = 0.0f;
        Vec3 previousPosition{0};
        Vec3 previousVelocity{0};
        f32 previousTumble = 0;
        f32 previousAge = 0;
        bool presentationCaptured = false;
        s32 potion = 0;
        f32 potency = 0.0f;
        f32 damage = 0.0f;
        f32 scale = 1.0f;
        const MissileSpec* spec = nullptr;
        const TreeModel* model = nullptr;
        u32 effect = 0;
        u32 rider = 0; ///< the effect riding it, moved with it and finished with it
        MissileWallSound wallSound = MissileWallSound::Level;
        u32 flags = 0;
        std::vector<s32> pierced;
        std::optional<MissileRelease> release;
        MissileStreak streak;
        std::optional<MissilePotionRelease> potionRelease;
        MultiplayerMode multiplayer = MultiplayerMode::Normal;
        f32 playerHitGap = 0.25f;
        bool breaksPotions = true;
        struct PlayerContact {
            s32 player = -1;
            f32 remaining = 0;
            bool operator==(const PlayerContact&) const = default;
            template <class D> friend void visitNativeState(D& digest, const PlayerContact& value) {
                digest.template fields<2>(value);
            }
        };
        std::vector<PlayerContact> playerContacts;
        template <class D> friend void visitNativeState(D& digest, const Missile& value) {
            const auto& [instance, continuity, owner, position, velocity, tumble, age,
                         previousPosition, previousVelocity, previousTumble, previousAge,
                         presentationCaptured, potion, potency, damage, scale, spec, model, effect,
                         rider, wallSound, flags, pierced, release, streak, potionRelease,
                         multiplayer, playerHitGap, breaksPotions, playerContacts] = value;
            digest.values(instance, continuity, owner, position, velocity, tumble, age,
                          previousPosition, previousVelocity, previousTumble, previousAge,
                          presentationCaptured, potion, potency, damage, scale);
            digest.flag(spec != nullptr);
            if (spec) {
                // The immutable specification can be stack-owned in physics-only
                // tests. Compare its complete value, not its allocation identity.
                digest.template fields<7>(*spec);
            }
            digest.resource(model);
            digest.values(effect, rider, wallSound, flags, pierced, release, streak, potionRelease,
                          multiplayer, playerHitGap, breaksPotions, playerContacts);
        }
    };

    /** Original immutable specs and prebound artwork, not live shooter slots.
     * A nonzero ID names the exact spec/model/streak-texture tuple. Invisible
     * missiles still need their actual spec. All resources outlive clear(). */
    struct Resource {
        u32 id = 0;
        const MissileSpec* spec = nullptr;
        const TreeModel* model = nullptr;
        const Texture* streak = nullptr;
    };
    struct Checkpoint {
        static constexpr usize kMaxMissiles = 4096;
        static constexpr usize kMaxEvents = 65536;
        static constexpr usize kMaxContacts = 65536;
        static constexpr usize kMaxResources = 4096;
        struct Flight {
            u64 instance = 0;
            u32 continuity = 1;
            s32 owner = 0;
            Vec3 position{0}, velocity{0};
            f32 tumble = 0, age = 0;
            Vec3 previousPosition{0}, previousVelocity{0};
            f32 previousTumble = 0, previousAge = 0;
            bool presentationCaptured = false;
            s32 potion = 0;
            f32 potency = 0, damage = 0, scale = 1;
            u32 resource = 0, effect = 0, rider = 0;
            MissileWallSound wallSound = MissileWallSound::Level;
            u32 flags = 0;
            std::vector<s32> pierced;
            std::optional<MissileRelease> release;
            Color streakColor = Color::white();
            f32 streakForward = 0;
            std::optional<MissilePotionRelease> potionRelease;
            MultiplayerMode multiplayer = MultiplayerMode::Normal;
            f32 playerHitGap = 0.25f;
            bool breaksPotions = true;
            std::vector<Missile::PlayerContact> playerContacts;
            bool operator==(const Flight&) const = default;
        };
        struct Impact {
            Vec3 position{0};
            s32 owner = 0, potion = 0;
            f32 potency = 0, damage = 0;
            s32 target = -1;
            u32 effectResource = 0; ///< zero is empty; otherwise the trusted spec's impactTree
            MissileWallSound wallSound = MissileWallSound::Level;
            u32 flags = 0;
            s32 node = -1, worldObject = -1;
            bool liquid = false;
            s32 player = -1;
            Vec3 direction{0};
            bool stun = false;
            f32 playerHitGap = 0.25f;
            bool operator==(const Impact&) const = default;
        };
        std::vector<Flight> missiles;
        std::vector<Impact> impacts;
        std::vector<VisualHistory> history;
        EffectTrees::PoolCheckpoint visuals;
        u64 nextInstance = 1;
        f32 ricochetIn = 0;
        bool visualsBound = false;
        bool valid() const;
        bool operator==(const Checkpoint&) const = default;
    };
    class Prepared {
    public:
        Prepared(Prepared&&) noexcept;
        Prepared& operator=(Prepared&&) noexcept;
        ~Prepared();
        Prepared(const Prepared&) = delete;
        Prepared& operator=(const Prepared&) = delete;

    private:
        friend class PlayerMissiles;
        struct Data;
        explicit Prepared(std::unique_ptr<Data> data);
        std::unique_ptr<Data> m_data;
    };
    /** Complete gameplay and visual state, including undelivered impacts. Bounds
     * refuse rollback capture, never truncate native gameplay. Render binding
     * mode and fallback lenders must match the destination's trusted setup. */
    std::optional<Checkpoint>
    checkpoint(std::span<const Resource> resources,
               std::span<const EffectTrees::PoolResource> effects = {}) const;
    std::optional<Prepared>
    prepareRestore(const Checkpoint& state, std::span<const Resource> resources,
                   std::span<const EffectTrees::PoolResource> effects = {}) const;
    bool canCommit(const Prepared& state) const noexcept;
    /** Prepare/preflight/commit synchronously with no intervening gameplay or
     * resource teardown. Commit allocates nothing and dispatches no callbacks. */
    bool commitRestore(Prepared&& state) noexcept;
    bool restore(const Checkpoint& state, std::span<const Resource> resources,
                 std::span<const EffectTrees::PoolResource> effects = {});

    /** A missile's pace from the stat that throws it. */
    static f32 speedFor(s32 stat);
    /** What a missile does to what it hits, by the thrower's strength (or magic). */
    static f32 damageFor(s32 stat);
    /** A point hit's cooldown comes from its impact animation's frame count at 30 Hz. */
    static f32 hitGap(const ItemArchive* archive, std::string_view tree);
    /** How far a throw reaches when the attack had been going `attackSeconds`. */
    static f32 reachFor(f32 attackSeconds);
    /** The velocity that sets a missile off along `direction` to come down at its reach. */
    static Vec3 launchVelocity(const Vec3& direction, f32 speed, f32 reach, f32 weight);
    /** Builds the same release as the live arsenal, without starting it. The
     * returned specs are stable class-table entries; visual pointers are empty. */
    static std::vector<MissileLaunch> weaponLaunches(const WeaponThrow& weapon);
    static FlightCheckpoint flightCheckpoint(const Missile& missile);
    /** Correct an existing plain shot in an unbound physics-only pool. Rejects
     * expired/invalid flight or any shot whose omitted impact state matters. */
    bool restoreFlight(u64 instance, const FlightCheckpoint& checkpoint);
    /** A native bottle in an unbound pool. It cannot carry piercing/reflection,
     * player contacts or ordinary-weapon provenance. Impact queues remain private. */
    bool restorePotionFlight(u64 instance, const FlightCheckpoint& checkpoint);
    /** Restore cosmetic counters only in a private physics-only pool. */
    bool restoreVisualFlight(u64 instance, const ParticleFlight& history);

    /** Sets a missile flying; false when the launch names no spec. */
    bool launch(const MissileLaunch& launch);
    /** Same native initialization/history as launch, using immutable artwork
     * prepared at stage load. No lazy binding or device calls occur. Failure
     * after a body/rider start requires the caller's whole-pool undo. */
    bool launchPrepared(const MissileLaunch& launch, const EffectTrees::Prepared& body,
                        const EffectTrees::Prepared* rider = nullptr);
    /** Binds rendering separately from physics. Texture lenders outlive clear(). */
    void bindVisuals(RenderDevice& device, std::span<TextureSet* const> lenders = {});
    const EffectTrees& visuals() const { return m_visuals; }
    std::span<const VisualHistory> visualHistory() const { return m_sim.visualHistory; }
    const VisualHistory* visualHistory(u32 effect) const;
    /** Flies every missile on by `seconds`; those a wall or floor stops are taken away. */
    void update(f32 seconds, const WorldCollision* collision,
                std::span<const MissileTarget> targets = {},
                std::span<const MissilePlayer> players = {});
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const CameraFrame* camera = nullptr, f32 frameBlend = -1.0f) const;
    void clear();

    usize count() const { return m_sim.missiles.size(); }
    const Missile& missile(usize index) const { return m_sim.missiles[index]; }
    /** Where missiles were stopped since the last call; each is handed out once. */
    std::vector<MissileImpact> takeImpacts();
    /** The directions of `shots` missiles about `direction`: fifteen degrees apart. */
    static std::vector<Vec3> spread(const Vec3& direction, s32 shots);
    static constexpr f32 kSpreadStep = 0.2617994f; ///< fifteen degrees
    /** Model space (flying along +z) to the world, for a missile. */
    static Mat4 transformOf(const Missile& missile, f32 frameBlend = 1.0f);

private:
    bool launchWithVisuals(const MissileLaunch& launch, const EffectTrees::Prepared* body,
                           const EffectTrees::Prepared* rider, bool prepared);
    struct Simulation {
        u64 nextInstance = 1; ///< never reset by clear; network identities cannot be recycled
        std::vector<Missile> missiles;
        std::vector<MissileImpact> impacts;
        std::vector<VisualHistory> visualHistory;
        f32 ricochetIn = 0;
        template <class D> friend void visitNativeState(D& digest, const Simulation& value) {
            digest.template fields<5>(value);
        }
    };
    Simulation m_sim;

public:
    class LocalSnapshot {
    private:
        friend class PlayerMissiles;
        Simulation m_state;
        EffectTrees::LocalSnapshot m_visuals;
        RenderDevice* m_device = nullptr;
        std::weak_ptr<const u8> m_owner;
        u64 m_epoch = 0;
    };
    /** Native value history. Borrowed specs, archives and attack tables must
     * remain alive for this stage epoch. */
    void saveLocal(LocalSnapshot& state) const;
    bool canRestoreLocal(const LocalSnapshot& state) const;
    bool restoreLocal(const LocalSnapshot& state);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& state) const;
    bool registerNativeResources(NativeStateResources& resources) const;

private:
    RenderDevice* m_device = nullptr;
    EffectTrees m_visuals;
    std::shared_ptr<const u8> m_restoreOwner = std::make_shared<const u8>(0);
    u64 m_restoreGeneration = 1;
    u64 m_localEpoch = 1;
};

} // namespace gdl::game
