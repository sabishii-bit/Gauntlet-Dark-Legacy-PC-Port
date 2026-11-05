#include "game/level/WorldDestruction.h"

#include <utility>

#include "engine/io/NativeStateDigest.h"

namespace gdl::game {
namespace {
constexpr u32 kKindMask = 0xf0000;
constexpr u32 kExplosive = 0x40000;
constexpr u32 kLoopingCart = 0x50000;
} // namespace

bool WorldDestruction::appendDigest(NativeStateDigest& digest, const Simulation& state) const {
    digest.string("WorldDestruction/1");
    digest.wide(m_objects.size());
    for (const auto& object : m_objects) {
        digest.fields<9>(object);
    }
    digest.values(m_controlled, state);
    return digest.good();
}

bool WorldDestruction::appendNativeDigest(NativeStateDigest& digest) const {
    return appendDigest(digest, m_sim);
}

bool WorldDestruction::appendLocalDigest(NativeStateDigest& digest,
                                         const LocalSnapshot& saved) const {
    // Guard the native snapshot roster too; owner/epoch are admission, not peer state.
    [[maybe_unused]] const auto& [state, binding, owner] = saved;
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    return appendDigest(digest, saved.m_state);
}

void WorldDestruction::saveLocal(LocalSnapshot& saved) const {
    saved.m_state = m_sim;
    saved.m_binding = m_checkpointBinding;
    saved.m_owner = this;
}

bool WorldDestruction::canRestoreLocal(const LocalSnapshot& saved) const {
    return saved.m_owner == this && m_checkpointBinding &&
           saved.m_binding.lock() == m_checkpointBinding;
}

bool WorldDestruction::equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const {
    return canRestoreLocal(left) && canRestoreLocal(right) && left.m_state == right.m_state;
}

bool WorldDestruction::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_state;
    return true;
}

void WorldDestruction::clear() {
    m_checkpointBinding = std::make_shared<const u8>(0);
    m_objects.clear();
    m_sim.destroyed.clear();
    m_controlled.clear();
    m_sim.explosions.clear();
}

bool WorldDestruction::valid(s32 object) const {
    return object >= 0 && static_cast<usize>(object) < m_objects.size();
}

bool WorldDestruction::below(usize descendant, s32 ancestor) const {
    auto at = static_cast<s32>(descendant);
    for (usize guard = 0; valid(at) && guard < m_objects.size(); ++guard) {
        if (at == ancestor) {
            return true;
        }
        at = m_objects[static_cast<usize>(at)].parent;
    }
    return false;
}

void WorldDestruction::bind(const WorldLayout& layout) {
    clear();
    m_objects = layout.objects();
    m_sim.destroyed.resize(m_objects.size(), false);
    std::vector<bool> controlled(m_objects.size(), false);
    for (usize i = 0; i < m_objects.size(); ++i) {
        const u32 kind = m_objects[i].flags & kKindMask;
        if (kind != kExplosive && kind != kLoopingCart) {
            continue;
        }
        auto root = static_cast<s32>(i);
        for (usize guard = 0;
             guard < m_objects.size() && valid(m_objects[static_cast<usize>(root)].parent);
             ++guard) {
            root = m_objects[static_cast<usize>(root)].parent;
        }
        for (usize descendant = 0; descendant < m_objects.size(); ++descendant) {
            if (below(descendant, root)) {
                controlled[descendant] = true;
            }
        }
    }
    for (usize i = 0; i < controlled.size(); ++i) {
        if (controlled[i]) {
            m_controlled.push_back(i);
        }
    }
}

bool WorldDestruction::destroyed(s32 object) const {
    return valid(object) && m_sim.destroyed[static_cast<usize>(object)];
}

void WorldDestruction::showSubtree(s32 object, bool visible, WorldScene& scene) {
    for (const usize descendant : m_controlled) {
        if (below(descendant, object)) {
            scene.setObjectVisible(descendant, visible);
        }
    }
}

bool WorldDestruction::wouldExplode(s32 object) const {
    if (!valid(object) || destroyed(object)) {
        return false;
    }
    u32 flags = 0;
    auto at = object;
    for (usize guard = 0; valid(at) && guard < m_objects.size(); ++guard) {
        flags |= m_objects[static_cast<usize>(at)].flags;
        at = m_objects[static_cast<usize>(at)].parent;
    }
    const u32 kind = flags & kKindMask;
    return kind == kExplosive || kind == kLoopingCart;
}

bool WorldDestruction::explode(s32 object, const Vec3& position, WorldScene& scene,
                               WorldCollision& collision) {
    if (!wouldExplode(object)) {
        return false;
    }
    m_sim.explosions.push_back(position);
    auto at = object;
    for (usize guard = 0; valid(at) && guard < m_objects.size(); ++guard) {
        m_sim.destroyed[static_cast<usize>(at)] = true;
        showSubtree(at, false, scene);
        collision.setSolid(at, false);
        at = m_objects[static_cast<usize>(at)].parent;
    }
    return true;
}

void WorldDestruction::update(std::span<const WorldAnimator::CycleEvent> events, WorldScene& scene,
                              WorldCollision& collision) {
    // Presentation is consumed in the same frame. Menus/attract sequences that do not
    // process gameplay effects must not accumulate delayed damaging explosions.
    m_sim.explosions.clear();
    for (const auto& event : events) {
        if (!valid(event.object)) {
            continue;
        }
        const auto index = static_cast<usize>(event.object);
        if (event.wrapped) {
            if ((m_objects[index].flags & kKindMask) == kLoopingCart) {
                explode(event.object, Vec3{scene.worldTransform(index)[3]}, scene, collision);
            }
        } else if (m_sim.destroyed[index]) {
            m_sim.destroyed[index] = false;
            showSubtree(event.object, true, scene);
            collision.setSolid(event.object, true);
        }
    }
}

std::vector<Vec3> WorldDestruction::takeExplosions() {
    return std::exchange(m_sim.explosions, {});
}
} // namespace gdl::game
