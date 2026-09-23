#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/enemies/Critters.h"
#include "game/enemies/LegendItems.h"

namespace gdl::game {

/** A step of a legend item's rite the game shows: which, and whose item it is. */
struct LegendEvent {
    LegendCue cue = LegendCue::Brandished;
    int player = -1;
    int realm = 0;
};

/** The boss a realm keeps: which, and how it stands. */
struct BossView {
    int kind = -1;    ///< the original's kind, 34 the dragon to 44 the garm
    std::string name; ///< "LICH"
    float health = 0.0f;
    float maxHealth = 1.0f;
    bool awake = false;
    bool alive = false;
    float fraction() const { return maxHealth > 0.0f ? std::max(health, 0.0f) / maxHealth : 0.0f; }
};

/**
 * A realm's boss. It fights by the same move table as the great ones (the original keeps
 * it in the same pool, but a boss is its own thing: one to a level, with its own name, a
 * sleep it wakes from when the party comes within its threshold, a health meter, and a
 * value in experience paid to everyone), so it keeps a fighter of its own rather than
 * sharing the critters' pool. A legend item brought to it is thrown as it rises (the
 * `LegendRite`), and its weakness is put on the fighter: a share of its health, a freeze,
 * a blinding or a curb on its attacks.
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
    bool spawn(int kind, const Vec3& position, float yaw, float wakeDistance = 0.0f);

    /** The realm of the legend item that weakens this boss, or nought. */
    int legendRealm() const { return legendRealmOf(m_kind); }
    /** Begins the rite of the boss's legend item, carried by `player`; false when the boss
     * has none, or it is already begun. */
    bool bringLegend(int player);
    /** The Dragon's ice axe acts on impact, not on the request to throw it.
     * Repeated impacts are ignored. Appearance is supplied separately at draw time. */
    void landLegend();
    const LegendRite& legend() const { return m_rite; }

    void update(int ticks, float seconds, std::span<const EnemyView> players);
    std::vector<CritterBlow> takeBlows();
    std::vector<CritterLoss> takeLosses();
    std::vector<CritterCue> takeCues() { return m_fighter.takeCues(); }
    std::vector<CritterShot> takeShots() { return m_fighter.takeShots(); }
    /** What its death threw out: the coins it spews. */
    std::vector<CritterSpew> takeSpews() { return m_fighter.takeSpews(); }
    std::vector<LegendEvent> takeLegendEvents();
    void hurt(const EnemyHit& hit);
    bool frozen() const;
    bool blinded() const;
    bool curbed() const;

    std::vector<MissileTarget> targets() const;
    std::optional<int> struckBy(const Vec3& from, const Vec3& to, float radius) const;
    bool within(const Vec3& centre, float radius) const;
    bool reachedBy(const Vec3& centre, float radius, float arc, const Vec3& facing) const;
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const Texture* frozenTexture = nullptr) const;

    bool present() const { return m_id.has_value(); }
    BossView view() const;
    /** How its meter is laid out, or null without a boss. */
    const CritterMeter* meter() const;
    /** Its archive, whose textures the meter is drawn from; null without a boss. */
    ItemArchive* archive();
    const Vec3* position() const;
    std::optional<Mat4> nodeTransform(std::string_view node) const;
    std::optional<Mat4> rootTransform() const;
    /** The way it faces, and how big it stands; nought without a boss. */
    float facing() const;
    float radius() const;
    /** How high its body's centre stands, which the fight's camera keeps in view. */
    float height() const;
    /** The tracking camera anchor includes the model root and vertical drift. */
    Vec3 cameraOffset() const;
    const std::optional<Vec3>& cameraBase() const { return m_cameraBase; }
    std::string_view moveName() const;
    /** The id targets and sweeps name the boss by. */
    static constexpr int kTargetId = 0;

private:
    void stageLegend(int ticks);
    void strikeWithLegend();

    Critters m_fighter; ///< holds the one boss
    std::optional<int> m_id;
    std::optional<Vec3> m_cameraBase;
    int m_kind = -1;
    std::string m_name;
    bool m_awake = false;
    float m_wakeDistance = 0.0f;
    LegendRite m_rite;
    bool m_roarAsked = false;
    std::vector<LegendEvent> m_legendEvents;
};

} // namespace gdl::game
