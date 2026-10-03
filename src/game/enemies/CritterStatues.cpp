#include "game/enemies/CritterStatues.h"

#include <cmath>
#include <utility>

#include "engine/core/Types.h"

namespace gdl::game {
std::string_view CritterStatues::treeOf(CombatantKind kind) {
    switch (kind) {
    case CombatantKind::Golem: return kGolemTree;
    case CombatantKind::Gargoyle: return kGargoyleTree;
    default: return {};
    }
}

s32 CritterStatues::activeTicks(s32 frames, s32 rate) {
    const f32 scale = rate > 0 ? static_cast<f32>(rate) * kFramesPerRateUnit : 1.0f;
    const auto whole = static_cast<s32>(std::floor(0.5f + static_cast<f32>(frames) * scale));
    return whole * kTicksPerActiveOn;
}

bool CritterStatues::add(RenderDevice& device, ItemArchive& archive, const Placement& placement,
                         const WorldCollision* collision) {
    std::string_view tree = treeOf(placement.kind);
    if (placement.enemy && placement.enemy->kind == kDeathKind) {
        tree = placement.enemy->tier == 2 ? "DEATHSTATUE2" : "DEATHSTATUE1";
    }
    if (tree.empty()) {
        return false;
    }
    auto statue = std::make_unique<Statue>();
    statue->placement = placement;
    if (!statue->figure.place(device, archive, tree, placement.instance, collision)) {
        return false;
    }
    statue->figure.play(kIdleSequence, true);
    m_statues.push_back(std::move(statue));
    return true;
}

void CritterStatues::clear() {
    m_statues.clear();
    m_risen.clear();
}

void CritterStatues::wake(usize index) {
    if (index < m_statues.size()) {
        m_statues[index]->woken = true;
    }
}

std::optional<std::pair<usize, f32>> CritterStatues::nearestAsleep(const Vec3& spot) const {
    std::optional<std::pair<usize, f32>> nearest;
    for (usize i = 0; i < m_statues.size(); ++i) {
        const Statue& statue = *m_statues[i];
        const f32 distance = glm::distance(statue.figure.position(), spot);
        if (!statue.woken && (!nearest.has_value() || distance < nearest->second)) {
            nearest = std::pair{i, distance};
        }
    }
    return nearest;
}

Obstacle CritterStatues::obstacleOf(const Statue& statue) {
    Obstacle cylinder;
    cylinder.centre = statue.figure.position();
    cylinder.yaw = statue.figure.yaw();
    cylinder.cylinderRadius = statue.placement.radius;
    cylinder.halfAcross = statue.placement.radius;
    cylinder.halfAlong = statue.placement.radius;
    cylinder.height = statue.placement.height;
    return cylinder;
}

Vec3 CritterStatues::touch(const Vec3& position, f32 radius, f32 height) {
    Vec3 stood = position;
    for (auto& statue : m_statues) {
        const Obstacle cylinder = obstacleOf(*statue);
        // The wake probe uses placement sight, independently of the solid body's radius.
        if (statue->placement.sight >= 0.0f) {
            const Vec3 away = stood - cylinder.centre;
            const f32 reach = statue->placement.sight + radius;
            if (away.x * away.x + away.z * away.z > reach * reach ||
                std::abs(away.y) > statue->placement.height + 0.5f * height) {
                continue;
            }
            statue->woken = true;
        }
        if (cylinder.touchedBy(stood, radius, 0.0f)) {
            stood = cylinder.pushOut(stood, radius);
        }
    }
    return stood;
}

std::vector<Obstacle> CritterStatues::obstacles() const {
    std::vector<Obstacle> out;
    out.reserve(m_statues.size());
    for (const auto& statue : m_statues) {
        out.push_back(obstacleOf(*statue));
    }
    return out;
}

std::vector<MissileTarget> CritterStatues::targets() const {
    std::vector<MissileTarget> out;
    out.reserve(m_statues.size());
    for (usize i = 0; i < m_statues.size(); ++i) {
        const Statue& statue = *m_statues[i];
        out.push_back(MissileTarget{static_cast<s32>(i), statue.figure.position(),
                                    statue.placement.radius, statue.placement.height});
    }
    return out;
}

void CritterStatues::update(s32 ticks, f32 seconds, const Seen& seen) {
    for (auto& statue : m_statues) {
        // Out of view nothing happens to it: neither the waking nor its countdown.
        if (!statue->woken ||
            (seen && !seen(statue->figure.position(), statue->placement.viewRadius))) {
            continue;
        }
        if (!statue->rising) {
            // The ACTIVE sequence starts, and its ticks with it: the record's activeOn, else
            // the sequence's own length (ProcessItems' activetime).
            statue->rising = true;
            statue->figure.play(kActiveSequence, false);
            const TreeSequenceInfo* active = statue->figure.sequenceInfo(kActiveSequence);
            statue->ticksLeft = 0;
            if (statue->placement.activeOn > 0) {
                statue->ticksLeft = statue->placement.activeOn * kTicksPerActiveOn;
            } else if (active != nullptr) {
                statue->ticksLeft = activeTicks(active->frames, active->frameRate);
            }
            continue;
        }
        statue->figure.update(seconds);
        statue->ticksLeft -= ticks;
        if (statue->ticksLeft <= 0) {
            m_risen.push_back(statue->placement);
            statue.reset();
        }
    }
    std::erase_if(m_statues, [](const std::unique_ptr<Statue>& statue) { return !statue; });
}

std::vector<CritterStatues::Placement> CritterStatues::takeRisen() {
    return std::exchange(m_risen, {});
}

void CritterStatues::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                          const CameraFrame* camera) const {
    for (const auto& statue : m_statues) {
        statue->figure.draw(device, clip, lighting, 1.0f, 1.0f, camera);
    }
}
} // namespace gdl::game
