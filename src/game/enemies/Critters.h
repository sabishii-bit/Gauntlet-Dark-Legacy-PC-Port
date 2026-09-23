#pragma once
#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/core/Types.h"

#include "game/enemies/Combatant.h"

namespace gdl::game {
inline constexpr s32 kGolemCritter = 3;
inline constexpr s32 kGargoyleCritter = 7;
inline constexpr s32 kGeneralCritter = 8;
inline constexpr s32 kBossCritter = 4;

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
    void open(RenderDevice& device, const std::filesystem::path& unpackedRoot,
              const WorldCollision* collision, const EnemyScales& scales, char realm);
    void close();

    /** Stands one of `kind` (a golem, a general, or a gargoyle by its form: "GAR_EAGL")
     * at `position` facing `yaw`. Bosses use their own encounter owner. Nullopt when its data or
     * archive is missing or there is no room. */
    std::optional<s32> spawn(s32 kind, const Vec3& position, f32 yaw, std::string_view form = "");

    void update(s32 ticks, f32 seconds, std::span<const EnemyView> players);
    std::vector<CombatBlow> takeBlows();
    std::vector<CombatLoss> takeLosses();
    /** The effects and sounds set off since the last call. */
    std::vector<CombatCue> takeCues();
    /** What the deaths since the last call threw out. */
    std::vector<CombatSpew> takeSpews();
    std::vector<CombatShot> takeShots();

    void hurt(s32 id, const EnemyHit& hit);
    /** Stops it where it stands, its animation with it, for `ticks`. */
    void freeze(s32 id, s32 ticks);
    /** Takes its targets from it for `ticks`, over which it turns at a tenth of its rate. */
    void blind(s32 id, s32 ticks);
    /** Refuses its curbed attacks (those whose harm is flagged so) while `seconds` is over
     * nought; nought lifts the curb. */
    void curb(s32 id, f32 seconds);
    void resize(s32 id, f32 scale);
    /** Keeps it to its stance between moves while `held`. */
    void hold(s32 id, bool held);
    /** Has it roar as soon as its move is over. */
    void roar(s32 id);
    std::vector<MissileTarget> targets() const;
    std::optional<s32> struckBy(const Vec3& from, const Vec3& to, f32 radius) const;
    std::vector<s32> within(const Vec3& centre, f32 radius) const;
    std::vector<s32> reachedBy(const Vec3& centre, f32 radius, f32 arc, const Vec3& facing) const;

    /** Optional frozen skin is borrowed for this draw only. */
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const Texture* frozenTexture = nullptr) const;

    usize count() const;
    bool alive(s32 id) const;
    bool dying(s32 id) const;
    s32 kindOf(s32 id) const;
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
    std::optional<s32> spawnGeneral(const Vec3& position, f32 yaw);
    std::optional<s32> spawnGargoyle(const Vec3& position, f32 yaw, std::string_view form = {});

private:
    std::optional<s32> spawn(const CombatantDefinition& definition, const Vec3& position, f32 yaw);
    CombatantAssets* stockFor(const CombatantDefinition& definition);
    void collect(Combatant& actor);
    RenderDevice* m_device = nullptr;
    std::filesystem::path m_root;
    const WorldCollision* m_collision = nullptr;
    EnemyScales m_scales;
    char m_realm = 'G';
    std::vector<std::unique_ptr<CombatantAssets>> m_stocks;
    std::array<Combatant, kMost> m_critters;
    std::vector<CombatBlow> m_blows;
    std::vector<CombatLoss> m_losses;
    std::vector<CombatCue> m_cues;
    std::vector<CombatSpew> m_spews;
    std::vector<CombatShot> m_shots;
    f32 m_textureFrames = 0;
};
} // namespace gdl::game
