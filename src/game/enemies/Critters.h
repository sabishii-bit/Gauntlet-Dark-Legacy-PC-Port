#pragma once
#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "engine/assets/TextureSet.h"
#include "engine/core/Types.h"

#include "game/enemies/Combatant.h"

namespace gdl::game {
/** Stable-id population of great creatures. Owns shared family assets and preserves
 * cross-family collision/update order; individual combat belongs to Combatant. */
class Critters {
public:
    static constexpr s32 kMost = 16;
    static constexpr f32 kBlockShare = Combatant::kBlockShare; ///< what gets through a block
    static constexpr f32 kKillShare =
        Combatant::kKillShare; ///< of the value, to everyone, on a kill
    static constexpr f32 kRoarAfter = Combatant::kRoarAfter; ///< damage taken before it roars
    static constexpr f32 kUnderLevelLoss =
        Combatant::kUnderLevelLoss; ///< experience lost a level under the place's
    static constexpr s32 kTicksPerSecond = Combatant::kTicksPerSecond;

    Critters() = default;
    Critters(const Critters&) = delete;
    Critters& operator=(const Critters&) = delete;
    Critters(Critters&&) = delete;
    Critters& operator=(Critters&&) = delete;
    ~Critters();

    /** Prepares for the level: its realm letter picks the golem's and general's costume. */
    /** The level's harmful surfaces, handed to each great one it stands; borrowed. */
    void setHazards(const HazardSurfaces* hazards) { m_hazards = hazards; }
    void open(RenderDevice& device, const std::filesystem::path& unpackedRoot,
              const WorldCollision* collision, const EnemyScales& scales, char realm,
              std::span<TextureSet* const> textureLenders = {}, std::string_view gargoyleForm = {});
    void close();

    /** The level's lookouts, for the kinds that walk a round of them (the generals). */
    void setLookouts(LookoutRoute lookouts) { m_lookouts = std::move(lookouts); }
    const LookoutRoute& lookouts() const { return m_lookouts; }

    /** Stands one of `kind` (a golem, a general, or a gargoyle by its form: "GAR_EAGL")
     * at `position` facing `yaw`. `sight` is its placement's sight radius, which at the
     * level's sight scale bounds the players a general takes on its round. Bosses use their
     * own encounter owner. Nullopt when its data or archive is missing or there is no room. */
    std::optional<s32> spawn(CombatantKind kind, const Vec3& position, f32 yaw,
                             std::string_view form = "", f32 sight = 0.0f);
    /** The archive one of `kind` comes from (loaded if need be), or null without it. */
    ItemArchive* archiveFor(CombatantKind kind, std::string_view form = "");

    /** `items` are what of the level's stands where they walk. */
    void update(s32 ticks, f32 seconds, std::span<const EnemyView> players,
                bool timeStopped = false, std::span<const CombatantObstacle> items = {},
                Enemies* swarm = nullptr);
    /** Carries the roster with moving floors even while gameplay is held. */
    void syncFloors();
    std::vector<CombatBlow> takeBlows();
    std::vector<CombatGrab> takeGrabs();
    std::vector<CombatLoss> takeLosses();
    /** The effects and sounds set off since the last call. */
    std::vector<CombatCue> takeCues();
    /** What the deaths since the last call threw out. */
    std::vector<CombatSpew> takeSpews();
    std::vector<CombatShot> takeShots();
    /** Their blows on what they walked into, since the last call. */
    std::vector<CombatantRam> takeRams() { return std::exchange(m_rams, {}); }
    std::vector<CombatPush> takePushes() { return std::exchange(m_pushes, {}); }

    f32 hurt(s32 id, const EnemyHit& hit);
    void damagedPlayer(s32 id, s32 player, f32 amount);
    /** Stops it where it stands, its animation with it, for `ticks`. */
    void freeze(s32 id, s32 ticks);
    /** Takes its targets from it for `ticks`, over which it turns at a tenth of its rate. */
    void blind(s32 id, s32 ticks);
    /** Refuses its curbed attacks (those whose harm is flagged so) while `seconds` is over
     * nought; nought lifts the curb. */
    void curb(s32 id, f32 seconds);
    void resize(s32 id, f32 scale);
    /** The scale the party's enemy shrinkers hold every one of them at (`EnemyShrink`). */
    void setShrink(f32 scale);
    /** Keeps it to its stance between moves while `held`. */
    void hold(s32 id, bool held);
    /** Has it roar as soon as its move is over. */
    void roar(s32 id);
    std::vector<MissileTarget> targets(bool solidOnly = false) const;
    std::optional<s32> struckBy(const Vec3& from, const Vec3& to, f32 radius) const;
    std::vector<s32> within(const Vec3& centre, f32 radius) const;
    std::vector<s32> reachedBy(const Vec3& centre, f32 radius, f32 arc, const Vec3& facing) const;

    /** The white skin a hard hit flashes over a body (`Combatant::flashing`); borrowed
     * until close(), none for no flash. */
    void setHitFlash(const Texture* texture) { m_hitFlash = texture; }
    /** Optional frozen skin is borrowed for this draw only. */
    void drawShadows(RenderDevice& device, const Mat4& clip, const Vec3& eye,
                     const WorldLighting& lighting, f32 presentationAlpha = -1) const;
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const Texture* frozenTexture = nullptr, const CameraFrame* camera = nullptr,
              f32 presentationAlpha = -1, bool healthBars = true) const;

    usize count() const;
    bool alive(s32 id) const;
    bool dying(s32 id) const;
    CombatantKind kindOf(s32 id) const;
    f32 healthOf(s32 id) const;
    f32 maxHealthOf(s32 id) const;
    /** Floor anchor, not the animation root (which includes the type's floorOffset). */
    const Vec3& positionOf(s32 id) const;
    f32 yawOf(s32 id) const;
    std::optional<Mat4> nodeTransformOf(s32 id, std::string_view node) const;
    std::optional<Mat4> rootTransformOf(s32 id) const;
    f32 radiusOf(s32 id) const;
    s32 targetOf(s32 id) const;
    /** The name of the move it is doing ("WALK", "ATTACK1L"). */
    std::string_view moveOf(s32 id) const;
    /** The type of the move it is doing, -1 with none. */
    s32 moveTypeOf(s32 id) const;
    /** Whether the move it is doing has played out. */
    bool moveDoneOf(s32 id) const;
    bool frozen(s32 id) const;
    bool blinded(s32 id) const;
    bool curbed(s32 id) const;
    f32 scaleOf(s32 id) const;
    const CritterData* dataOf(s32 id) const;
    /** The archive its body and textures came from, or null. */
    ItemArchive* archiveOf(s32 id);
    /** A gargoyle's form ("EAGL"), empty for the rest. */
    std::string formOf(s32 id) const;

    std::optional<s32> spawnGolem(const Vec3& position, f32 yaw);
    std::optional<s32> spawnGeneral(const Vec3& position, f32 yaw, f32 sight = 0.0f);
    std::optional<s32> spawnGargoyle(const Vec3& position, f32 yaw, std::string_view form = {});
    /** Whether it is on its round of the lookouts, and which it makes for. */
    bool patrolling(s32 id) const;
    s32 lookoutOf(s32 id) const;

private:
    std::optional<s32> spawn(const CombatantDefinition& definition, const Vec3& position, f32 yaw,
                             f32 sight = 0.0f);
    CombatantAssets* stockFor(const CombatantDefinition& definition);
    CombatantDefinition definitionOf(CombatantKind kind, std::string_view form) const;
    void collect(Combatant& actor);
    RenderDevice* m_device = nullptr;
    std::filesystem::path m_root;
    std::vector<TextureSet*> m_textureLenders; ///< stage owners outlive the population
    const WorldCollision* m_collision = nullptr;
    const HazardSurfaces* m_hazards = nullptr; ///< borrowed from the level
    EnemyScales m_scales;
    char m_realm = 'G';
    std::string m_gargoyleForm; ///< the level roster's form, shared by statues and live actors
    LookoutRoute m_lookouts;    ///< the level's, borrowed by the generals' rounds
    std::vector<std::unique_ptr<CombatantAssets>> m_stocks;
    std::array<Combatant, kMost> m_critters;
    std::vector<CombatBlow> m_blows;
    std::vector<CombatGrab> m_grabs;
    std::vector<CombatLoss> m_losses;
    std::vector<CombatCue> m_cues;
    std::vector<CombatSpew> m_spews;
    std::vector<CombatShot> m_shots;
    std::vector<CombatantRam> m_rams;
    std::vector<CombatPush> m_pushes;
    const Texture* m_hitFlash = nullptr; ///< borrowed from the level
};
} // namespace gdl::game
