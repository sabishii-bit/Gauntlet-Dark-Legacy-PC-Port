#include "game/level/WorldMotion.h"

#include <cmath>
#include <unordered_set>

namespace gdl::game {
bool WorldMotion::State::valid() const {
    if (collision.empty() || collision.size() > kMaxObjects || animations.size() > kMaxAnimations ||
        rotators.size() > Rotators::kMaxCursors || !triggers.valid()) {
        return false;
    }
    for (const u8 flags : collision) {
        if ((flags & ~(kSolid | kContactOnly | kFloorExitBlocked | kPlayerPassThrough)) != 0) {
            return false;
        }
    }
    const auto objectValid = [&](s32 object) {
        return object >= 0 && static_cast<usize>(object) < collision.size();
    };
    std::unordered_set<s32> animated;
    for (const auto& cursor : animations) {
        if (!cursor.valid() || !objectValid(cursor.object) ||
            !animated.insert(cursor.object).second) {
            return false;
        }
    }
    for (const auto& target : triggers.targets) {
        if (!objectValid(target.object)) {
            return false;
        }
    }
    std::unordered_set<s32> rotating;
    for (const auto& cursor : rotators) {
        if (!cursor.valid() || !objectValid(cursor.object) ||
            !rotating.insert(cursor.instance).second) {
            return false;
        }
    }
    return true;
}

void WorldMotion::bind(const WorldLayout& layout) {
    m_prepared = false;
    m_objects = layout.objects().size();
    m_pauseLoops = false;
    m_moving.clear();
    m_animator.bind(layout);
    // Motion depends only on authored target records. Floor-bound switch markers
    // and rendered pad models are deliberately absent from this private replay.
    m_triggers.bind(layout, m_animator, nullptr);
    m_rotators.bind(layout);
}

WorldMotion::State WorldMotion::capture(const WorldAnimator& animator,
                                        const LevelTriggers& triggers, const Rotators& rotators,
                                        const WorldCollision& collision, usize objectCount,
                                        bool pauseLoops) {
    State result;
    if (objectCount == 0 || objectCount > kMaxObjects) {
        return result;
    }
    result.animations = animator.checkpoint();
    result.triggers = triggers.motion();
    result.rotators = rotators.checkpoint();
    result.pauseLoops = pauseLoops;
    result.collision.reserve(objectCount);
    for (usize index = 0; index < objectCount; ++index) {
        const auto object = static_cast<s32>(index);
        result.collision.push_back(
            static_cast<u8>((collision.solid(object) ? kSolid : 0) |
                            (collision.contactOnly(object) ? kContactOnly : 0) |
                            (collision.ownFloorExitBlocked(object) ? kFloorExitBlocked : 0) |
                            (collision.ownPlayerPassThrough(object) ? kPlayerPassThrough : 0)));
    }
    return result;
}

WorldMotion::State WorldMotion::checkpoint(const WorldCollision& collision) const {
    return capture(m_animator, m_triggers, m_rotators, collision, m_objects, m_pauseLoops);
}

bool WorldMotion::prepare(const State& state, const SceneGeometry& geometry, WorldScene& scene,
                          WorldCollision& collision) {
    if (!state.valid() || state.collision.size() != m_objects ||
        geometry.objectCount != m_objects || !scene.acceptsGeometry(geometry) ||
        !m_triggers.acceptsMotion(state.triggers) || !m_rotators.accepts(state.rotators)) {
        return false;
    }
    auto animator = m_animator;
    if (!animator.restore(state.animations)) {
        return false;
    }
    // Validation above covers every operation below. Applying a cursor pose here
    // would jump a frame: the saved scene contains the pose BEFORE its clock advance.
    if (!scene.applyGeometry(geometry)) {
        return false;
    }
    m_animator = std::move(animator);
    m_triggers.restoreMotion(state.triggers);
    m_rotators.restore(state.rotators);
    m_moving.clear();
    for (usize index = 0; index < m_objects; ++index) {
        const auto object = static_cast<s32>(index);
        const u8 flags = state.collision[index];
        collision.setSolid(object, (flags & kSolid) != 0);
        collision.setContactOnly(object, (flags & kContactOnly) != 0);
        collision.setFloorExitBlocked(object, (flags & kFloorExitBlocked) != 0);
        collision.setPlayerPassThrough(object, (flags & kPlayerPassThrough) != 0);
        if (collision.moving(object)) {
            m_moving.push_back(object);
        }
    }
    m_pauseLoops = state.pauseLoops;
    sync(scene, collision);
    m_prepared = true;
    return true;
}

void WorldMotion::sync(WorldScene& scene, WorldCollision& collision) const {
    for (const s32 object : m_moving) {
        collision.setObjectTransform(object, scene.worldTransform(static_cast<usize>(object)));
    }
}

bool WorldMotion::beforeMotion(f32 seconds, WorldScene& scene, WorldCollision& collision) {
    if (!m_prepared || !std::isfinite(seconds) || seconds < 0 || seconds > 0.25f) {
        return false;
    }
    m_animator.step(seconds, scene, m_pauseLoops, m_triggers.heldTracks());
    sync(scene, collision);
    return true;
}

bool WorldMotion::afterMotion(f32 seconds, WorldScene& scene, WorldCollision& collision) {
    if (!m_prepared || !std::isfinite(seconds) || seconds < 0 || seconds > 0.25f) {
        return false;
    }
    m_triggers.advanceMotion(seconds, m_animator, scene, &collision);
    m_rotators.advanceMotion(seconds, scene);
    sync(scene, collision);
    return true;
}
} // namespace gdl::game
