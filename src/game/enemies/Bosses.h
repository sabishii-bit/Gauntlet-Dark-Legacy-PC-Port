#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/enemies/BossDefinition.h" // IWYU pragma: export
#include "game/enemies/Combatant.h"
#include "game/enemies/LegendItems.h"

namespace gdl::game {

/** A step of a legend item's rite the game shows: which, and whose item it is. */
struct LegendEvent {
    LegendCue cue = LegendCue::Brandished;
    s32 player = -1;
    s32 realm = 0;
};

/** The boss a realm keeps: which, and how it stands. */
struct BossView {
    s32 kind = -1;    ///< the original's kind, 34 the dragon to 44 the garm
    std::string name; ///< "LICH"
    f32 health = 0.0f;
    f32 maxHealth = 1.0f;
    bool awake = false;
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
              const WorldCollision* collision, const EnemyScales& scales, char realm);
    void close();

    /** Stands the boss of `kind` (`bossNameOf` it) at `position` facing `yaw`, asleep until
     * the party comes within `wakeDistance` (its table's threshold when nought). */
    bool spawn(s32 kind, const Vec3& position, f32 yaw, f32 wakeDistance = 0.0f);

    /** The realm of the legend item that weakens this boss, or nought. */
    s32 legendRealm() const { return legendRealmOf(m_kind); }
    /** Begins the rite of the boss's legend item, carried by `player`; false when the boss
     * has none, or it is already begun. */
    bool bringLegend(s32 player);
    /** The Dragon's ice axe and Genie's lamp act on impact, not on the request to throw them.
     * Repeated impacts are ignored. Appearance is supplied separately at draw time. */
    void landLegend();
    const LegendRite& legend() const { return m_rite; }

    void update(s32 ticks, f32 seconds, std::span<const EnemyView> players);
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
    std::vector<CombatCue> takeCues() { return m_fighter.takeCues(); }
    std::vector<CombatShot> takeShots() { return m_fighter.takeShots(); }
    /** What its death threw out: the coins it spews. */
    std::vector<CombatSpew> takeSpews() { return m_fighter.takeSpews(); }
    std::vector<LegendEvent> takeLegendEvents();
    void hurt(const EnemyHit& hit);
    bool frozen() const;
    bool blinded() const;
    bool curbed() const;

    std::vector<MissileTarget> targets() const;
    std::optional<s32> struckBy(const Vec3& from, const Vec3& to, f32 radius) const;
    bool within(const Vec3& centre, f32 radius) const;
    bool reachedBy(const Vec3& centre, f32 radius, f32 arc, const Vec3& facing) const;
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const Texture* frozenTexture = nullptr) const;

    bool present() const { return m_id.has_value(); }
    BossView view() const;
    /** How its meter is laid out, or null without a boss. */
    const HealthMeterDefinition* meter() const;
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
    const std::optional<Vec3>& cameraBase() const { return m_cameraBase; }
    std::string_view moveName() const;
    /** The id targets and sweeps name the boss by. */
    static constexpr s32 kTargetId = 0;

private:
    void stageLegend(s32 ticks);
    void strikeWithLegend();

    Combatant m_fighter;
    // Effects and projectiles can outlive their boss. Retain borrowed assets until close().
    std::vector<std::unique_ptr<CombatantAssets>> m_assets;
    RenderDevice* m_device = nullptr;
    std::filesystem::path m_root;
    const WorldCollision* m_collision = nullptr;
    EnemyScales m_scales;
    char m_realm = 'G';
    f32 m_textureFrames = 0;
    std::optional<s32> m_id;
    std::optional<Vec3> m_cameraBase;
    s32 m_kind = -1;
    std::string m_name;
    bool m_awake = false;
    f32 m_wakeDistance = 0.0f;
    LegendRite m_rite;
    bool m_roarAsked = false;
    std::vector<LegendEvent> m_legendEvents;
};

} // namespace gdl::game
