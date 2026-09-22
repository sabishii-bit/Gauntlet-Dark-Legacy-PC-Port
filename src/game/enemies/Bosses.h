#pragma once

#include <filesystem>
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
#include "game/enemies/Critters.h"

namespace gdl::game {

/** The boss a realm keeps: which, and how it stands. */
struct BossView {
    s32 kind = -1;         ///< the original's kind, 34 the dragon to 44 the garm
    std::string name;      ///< "LICH"
    f32 health = 0.0f;
    f32 maxHealth = 1.0f;
    bool awake = false;
    bool alive = false;
    f32 fraction() const { return maxHealth > 0.0f ? std::max(health, 0.0f) / maxHealth : 0.0f; }
};

/**
 * A realm's boss. It fights by the same move table as the great ones (the original keeps
 * it in the same pool, but a boss is its own thing: one to a level, with its own name, a
 * sleep it wakes from when the party comes within its threshold, a health meter, and a
 * value in experience paid to everyone), so it keeps a fighter of its own rather than
 * sharing the critters' pool.
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

    void update(s32 ticks, f32 seconds, std::span<const EnemyView> players);
    std::vector<CritterBlow> takeBlows();
    std::vector<CritterLoss> takeLosses();
    void hurt(const EnemyHit& hit);

    std::vector<MissileTarget> targets() const;
    std::optional<s32> struckBy(const Vec3& from, const Vec3& to, f32 radius) const;
    bool within(const Vec3& centre, f32 radius) const;
    bool reachedBy(const Vec3& centre, f32 radius, f32 arc, const Vec3& facing) const;
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;

    bool present() const { return m_id.has_value(); }
    BossView view() const;
    const Vec3* position() const;
    std::string_view moveName() const;
    /** The id targets and sweeps name the boss by. */
    static constexpr s32 kTargetId = 0;

private:
    Critters m_fighter; ///< holds the one boss
    std::optional<s32> m_id;
    s32 m_kind = -1;
    std::string m_name;
    bool m_awake = false;
    f32 m_wakeDistance = 0.0f;
};

} // namespace gdl::game
