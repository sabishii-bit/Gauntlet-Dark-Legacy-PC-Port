#include "game/enemies/Bosses.h"

#include <cmath>

namespace gdl::game {

void Bosses::open(RenderDevice& device, const std::filesystem::path& unpackedRoot,
                  const WorldCollision* collision, const EnemyScales& scales, char realm) {
    close();
    m_fighter.open(device, unpackedRoot, collision, scales, realm);
}

void Bosses::close() {
    m_fighter.close();
    m_id.reset();
    m_kind = -1;
    m_name.clear();
    m_awake = false;
    m_wakeDistance = 0.0f;
}

bool Bosses::spawn(s32 kind, const Vec3& position, f32 yaw, f32 wakeDistance) {
    const std::string_view name = bossNameOf(kind);
    if (name.empty() || m_id.has_value()) {
        return false;
    }
    m_id = m_fighter.spawn(kBossCritter, position, yaw, name);
    if (!m_id.has_value()) {
        return false;
    }
    m_kind = kind;
    m_name = name;
    m_awake = false;
    const CritterData* data = m_fighter.dataOf(*m_id);
    m_wakeDistance = wakeDistance;
    if (m_wakeDistance <= 0.0f && data != nullptr) {
        m_wakeDistance = data->wakeThreshold();
    }
    return true;
}

void Bosses::update(s32 ticks, f32 seconds, std::span<const EnemyView> players) {
    if (!m_id.has_value()) {
        return;
    }
    // Asleep until the party comes within its threshold; then it fights on until it falls.
    if (!m_awake) {
        const Vec3* at = position();
        for (const EnemyView& view : players) {
            if (at != nullptr && !view.hidden &&
                (m_wakeDistance <= 0.0f || glm::distance(view.position, *at) <= m_wakeDistance)) {
                m_awake = true;
            }
        }
        if (!m_awake) {
            return;
        }
    }
    m_fighter.update(ticks, seconds, players);
    if (m_fighter.count() == 0) {
        m_id.reset();
    }
}

std::vector<CritterBlow> Bosses::takeBlows() {
    return m_fighter.takeBlows();
}

std::vector<CritterLoss> Bosses::takeLosses() {
    return m_fighter.takeLosses();
}

void Bosses::hurt(const EnemyHit& hit) {
    if (m_id.has_value()) {
        m_awake = true; // struck, it wakes
        m_fighter.hurt(*m_id, hit);
    }
}

std::vector<MissileTarget> Bosses::targets() const {
    return m_fighter.targets();
}

std::optional<s32> Bosses::struckBy(const Vec3& from, const Vec3& to, f32 radius) const {
    return m_fighter.struckBy(from, to, radius);
}

bool Bosses::within(const Vec3& centre, f32 radius) const {
    return !m_fighter.within(centre, radius).empty();
}

bool Bosses::reachedBy(const Vec3& centre, f32 radius, f32 arc, const Vec3& facing) const {
    return !m_fighter.reachedBy(centre, radius, arc, facing).empty();
}

void Bosses::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const {
    m_fighter.draw(device, clip, lighting);
}

BossView Bosses::view() const {
    BossView view;
    view.kind = m_kind;
    view.name = m_name;
    view.awake = m_awake;
    if (m_id.has_value() && (m_fighter.alive(*m_id) || m_fighter.dying(*m_id))) {
        view.health = m_fighter.healthOf(*m_id);
        view.maxHealth = m_fighter.maxHealthOf(*m_id);
        view.alive = m_fighter.alive(*m_id);
    }
    return view;
}

const Vec3* Bosses::position() const {
    return m_id.has_value() ? &m_fighter.positionOf(*m_id) : nullptr;
}

std::string_view Bosses::moveName() const {
    return m_id.has_value() ? m_fighter.moveOf(*m_id) : std::string_view{};
}

} // namespace gdl::game
