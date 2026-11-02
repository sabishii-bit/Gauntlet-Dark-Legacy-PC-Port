#pragma once
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

#include "game/audio/LevelSoundscape.h"
#include "game/effects/EffectTrees.h"
#include "game/party/PlayerFigure.h"
#include "game/party/PlayerMissiles.h"
#include "game/party/PotionEmission.h"
#include "game/players/PlayerActor.h"
namespace gdl::game {
/** Owns player projectiles and thrown-potion models. Bound services are borrowed;
 * clear before releasing figures, effects, world collision or the weapons archive.
 * Models/missiles retain addresses inside this owner, so it must not move. */
class PlayerArsenal {
    struct CastEffect {
        s32 owner;
        u32 effect;
        template <class D> friend void visitNativeState(D& digest, const CastEffect& value) {
            digest.template fields<2>(value);
        }
    };
    struct Simulation {
        std::vector<CastEffect> casts;
        s32 nextPotionKind = 1;
        template <class D> friend void visitNativeState(D& digest, const Simulation& value) {
            digest.template fields<2>(value);
        }
    };

public:
    /** Same-stage native values; borrowed resources and output observers stay put.
     * Shared cast effects and player inventory belong to the enclosing scene. */
    class LocalSnapshot {
        friend class PlayerArsenal;
        Simulation m_state;
        PlayerMissiles::LocalSnapshot m_missiles;
        const PlayerArsenal* m_owner = nullptr;
        std::weak_ptr<const u8> m_binding;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;
    bool registerNativeResources(NativeStateResources& resources) const;

    static constexpr f32 kBurstPerPower = PotionEmission::kBurstPerPower;
    struct WeaponSound {
        SoundHandle handle = kNoSound;
        s32 owner = -1;
        MissileRelease release;
    };
    using SoundObserver = std::function<void(const WeaponSound&)>;
    /** Observe native release cues within a simulation scope. Nested scopes restore
     * their predecessor; the observer must not mutate this arsenal. */
    class SoundCapture {
    public:
        SoundCapture(PlayerArsenal* arsenal, SoundObserver observer);
        ~SoundCapture();
        SoundCapture(const SoundCapture&) = delete;
        SoundCapture& operator=(const SoundCapture&) = delete;

    private:
        PlayerArsenal* m_arsenal;
        SoundObserver m_previous;
    };
    struct Resources {
        RenderDevice& device;
        const ClassDataSet& classes;
        ItemArchive& weapons;
        const WorldCollision& collision;
        EffectTrees& effects;
        LevelSoundscape& audio;
        SoundPlayer* sounds;
        std::string_view wallHitSound; ///< resolved from the level's audio record
        bool tower = false;
        bool bossEncounter = false;
        ItemArchive* powerups = nullptr;              ///< borrowed potion-cast feedback archive
        const MultiplayerMode* multiplayer = nullptr; ///< sampled when a weapon is launched
        /** Optional imported-world item bridge. Ordinary GDL wall obstruction
         * remains feedback-only; the receiver accepts only authored destructibles. */
        std::function<bool(s32 object, f32 power, u32 flags, s32 player)> strikeMuzzleObject;
    };
    PlayerArsenal() = default;
    ~PlayerArsenal() = default;
    PlayerArsenal(const PlayerArsenal&) = delete;
    PlayerArsenal& operator=(const PlayerArsenal&) = delete;
    PlayerArsenal(PlayerArsenal&&) = delete;
    PlayerArsenal& operator=(PlayerArsenal&&) = delete;
    void bind(const Resources& resources, std::span<TextureSet* const> textureLenders = {});
    void clear();
    /** Samples current authoritative weapon tuning without launching or spending anything. */
    std::optional<PlayerMissiles::WeaponThrow> weaponThrow(const PlayerActor& actor,
                                                           f32 attackSeconds) const;
    struct PreparedWeapon {
        PlayerMissiles::WeaponThrow weapon;
        u32 soundFlags = 0; ///< sound priority uses unmodified inventory flags
        bool pojo = false;
        std::vector<MissileLaunch> missiles; ///< static physics specs, no visual bindings
    };
    enum class WeaponMode { Throw, Volley };
    struct WeaponCue {
        std::string_view name;                  ///< named level/common/ambient cue, when nonempty
        std::optional<u32> call = std::nullopt; ///< otherwise the bound figure's class voice call
        bool operator==(const WeaponCue&) const = default;
    };
    /** Native fan/element/class priority, without playing or decoding a voice. */
    static WeaponCue weaponCue(u32 weapon, const PlayerFigure& figure);
    /** Pure release preparation from trusted local actor/figure state. No artwork,
     * texture, audio, collision or inventory mutation occurs. An explicit release
     * belongs to a caller's native event; otherwise ordinary release latches supply
     * provenance. The caller must supply a canonical (fan ordinal zero) identity. */
    std::optional<PreparedWeapon>
    prepareWeapon(const PlayerActor& actor, const PlayerFigure& figure, const Vec3& direction,
                  f32 scale, bool spreads, std::optional<Vec3> target = std::nullopt,
                  std::optional<MissileRelease> explicitRelease = std::nullopt,
                  bool fixedFacing = false, WeaponMode mode = WeaponMode::Throw) const;
    /** Borrows the native streak texture and tint; may populate the texture cache. */
    MissileStreak weaponStreak(const PlayerActor& actor, bool superShot = false);
    /** Existing plain-weapon texture bank clock, absent before the bank's first use. */
    std::optional<f32> weaponTextureFrame(const PlayerFigure& figure) const;
    struct WeaponRider {
        ItemArchive* archive = nullptr;
        const TreeInfo* tree = nullptr;
        std::vector<TextureSet*> lenders;
        std::optional<f32> textureFrame;
    };
    /** Native elemental throw tree and its existing bank clock, without starting an effect. */
    std::optional<WeaponRider> weaponRider(const PlayerActor& actor, PlayerFigure& figure) const;
    void launchWeapon(const PlayerActor& actor, PlayerFigure* body, const Vec3& direction,
                      f32 scale, bool spreads, std::optional<Vec3> target = std::nullopt,
                      std::optional<MissileRelease> explicitRelease = std::nullopt,
                      bool fixedFacing = false, WeaponMode mode = WeaponMode::Throw);
    void launchSuperShot(PlayerActor& actor, PlayerFigure* body,
                         std::optional<Vec3> target = std::nullopt);
    void launchGauntlet(const PlayerActor& actor, PlayerFigure* body, bool left);
    void launchFamiliar(const PlayerActor& actor, PlayerFigure* body,
                        std::optional<Vec3> target = std::nullopt, bool fixedFacing = false);
    /** Native companion physics; stable addresses also identify trusted pool resources. */
    static const MissileSpec& familiarSpec(bool boss = false);
    const TreeModel& phoenixMissile() const { return m_phoenixShot; }
    /** Pure native companion volley, earned familiar first then Phoenix. Only
     * already-loaded artwork is borrowed; no muzzle query or release provenance
     * belongs to these silent shots. Empty means neither companion fires. */
    std::optional<std::vector<MissileLaunch>>
    prepareFamiliar(const PlayerActor& actor, PlayerFigure& figure,
                    std::optional<Vec3> target = std::nullopt, bool fixedFacing = false) const;
    std::optional<MissileImpact> usePotion(PlayerActor& actor);
    /** Unspecified magic advances one shared colour cycle for every caster and floor bottle. */
    s32 resolvePotionKind(s32 kind);
    void throwPotion(PlayerActor& actor, s32 heldTicks = 0, u64 generation = 0);
    /** Existing bound native bottle artwork; querying never loads a resource. */
    const TreeModel* potionModel(s32 kind) const;
    /** Keep short-lived cast effects attached without retaining a player address. */
    void followCaster(const PlayerActor& actor);
    void burstPotion(s32 kind, const Vec3& position, f32 power, bool castSound = true);
    struct ImpactCue {
        std::string name;
        Vec3 position{0};
        std::optional<f32> playerDistance; ///< absent for the native non-spatial wall cue
        f32 volume = 1;
        bool operator==(const ImpactCue&) const = default;
    };
    struct ImpactPresentation {
        ItemArchive* archive = nullptr;
        std::string_view tree;
        Vec3 position{0};
        EffectTrees::Setting setting;
        std::optional<ImpactCue> cue;
    };
    /** Resolve ordinary weapon collision artwork/audio without loading, allocating
     * effects, or playing anything. Potion bursts have their separate native path. */
    ImpactPresentation impactPresentation(const MissileImpact& impact,
                                          f32 playerDistance = 0) const;
    /** Present a collision once, without applying target damage or expiry effects. */
    void presentImpact(const MissileImpact& impact, f32 playerDistance = 0);
    f32 magicPowerOf(const PlayerActor& actor) const;
    /** The power a potion of `kind` goes off with from `actor`: their magic's, a tenth more
     * for a potion of their own colour. */
    f32 potionPowerOf(const PlayerActor& actor, s32 kind) const;
    static constexpr f32 kPotionDamage = PotionEmission::kDamage; ///< before the colour bonus
    PlayerMissiles& missiles() { return m_missiles; }
    const PlayerMissiles& missiles() const { return m_missiles; }

private:
    void appendBindingDigest(NativeStateDigest& digest) const;
    friend class PlayerArsenalCheckpoint;
    friend class CombatRollbackCheckpoint;
    MissileStreak weaponStreak(const PlayerActor& actor, const Texture* texture,
                               bool superShot = false) const;
    void bindWeaponAppearance(const PlayerActor& actor, PlayerFigure& figure,
                              PreparedWeapon& prepared, const MissileStreak& streak) const;
    /** Native ordinary fans share the first muzzle; later flight directions
     * never add separate launch-time wall queries. Caller has a bound owner. */
    Vec3 weaponMuzzle(const MissileLaunch& first, std::vector<WallContact>* contacts) const;
    std::shared_ptr<const u8> m_restoreOwner = std::make_shared<const u8>(0);
    u64 m_restoreGeneration = 1;
    void loadPotionModels();
    void presentPotion(const PotionEmission::Burst& burst, bool castSound);
    void healingCast(const PlayerActor& actor, f32 power);
    Simulation m_sim;
    std::shared_ptr<const u8> m_localBinding = std::make_shared<const u8>(0);
    std::optional<Resources> m_resources;
    std::array<TreeModel, 5> m_potionModels;
    TreeModel m_superShot;
    TreeModel m_phoenixShot;
    std::array<TreeModel, 2> m_gauntlets;
    PlayerMissiles m_missiles;
    SoundObserver m_soundObserver;
};
} // namespace gdl::game
