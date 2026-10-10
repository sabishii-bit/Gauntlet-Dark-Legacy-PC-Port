#include "game/netplay/CombatPlayback.h"

#include <algorithm>
#include <cmath>

namespace gdl::game {
namespace {
CombatAnimation between(const CombatAnimation& from, const CombatAnimation& to, f32 t) {
    auto result = from;
    if (from.generation != 0 && from.generation == to.generation && from.action == to.action &&
        from.sequence == to.sequence && to.frame >= from.frame) {
        result.frame = std::lerp(from.frame, to.frame, t);
        result.transition = std::lerp(from.transition, to.transition, t);
    }
    return result;
}

Mat4 placementBetween(const Mat4& start, const Mat4& end, f32 t) {
    if (start == end) {
        return start;
    }
    Mat3 from{start};
    Mat3 to{end};
    if (from == to) {
        // Reconstructing an unchanged basis through a quaternion perturbs its
        // bits. Static scene units must retain their exact authored transform;
        // translating platforms also need no rotation/scale reconstruction.
        Mat4 result = start;
        result[3] = glm::mix(start[3], end[3], t);
        return result;
    }
    Vec3 fromScale{0};
    Vec3 toScale{0};
    bool rotations = true;
    for (s32 axis = 0; axis < 3; ++axis) {
        fromScale[axis] = glm::length(from[axis]);
        toScale[axis] = glm::length(to[axis]);
        if (fromScale[axis] < 1e-6f || toScale[axis] < 1e-6f) {
            rotations = false;
            break;
        }
        from[axis] /= fromScale[axis];
        to[axis] /= toScale[axis];
    }
    const auto orthogonal = [](const Mat3& basis) {
        return std::abs(glm::determinant(basis) - 1) < 0.001f &&
               std::abs(glm::dot(basis[0], basis[1])) < 0.001f &&
               std::abs(glm::dot(basis[1], basis[2])) < 0.001f &&
               std::abs(glm::dot(basis[0], basis[2])) < 0.001f;
    };
    // Sheared/reflected attachments retain their authored basis until the next
    // checkpoint; they must not be reinterpreted as quaternion rotations.
    Mat4 result = start;
    if (rotations && orthogonal(from) && orthogonal(to)) {
        result = glm::scale(glm::mat4_cast(glm::slerp(glm::quat_cast(from), glm::quat_cast(to), t)),
                            glm::mix(fromScale, toScale, t));
    }
    result[3] = glm::mix(start[3], end[3], t);
    return result;
}
} // namespace

bool CombatPlayback::begin(PacketTransport::Connection host, u64 epoch) {
    if (!m_receiver.begin(host, epoch)) {
        return false;
    }
    m_motion.clear();
    m_history.clear();
    return m_motion.begin(host, epoch);
}
void CombatPlayback::clear() {
    m_receiver.clear();
    m_motion.clear();
    m_history.clear();
}
CombatReplica::Admission CombatPlayback::receive(PacketTransport::Connection sender,
                                                 std::span<const u8> bytes) {
    const auto admitted = m_receiver.receive(sender, bytes);
    if (admitted == CombatReplica::Admission::Committed) {
        const auto* snapshot = m_receiver.latest();
        if (snapshot != nullptr) {
            // Reuse the motion path's cut/seat-identity checks. This small local
            // encoding does not send a second packet or admit another authority.
            if (const auto motion = MotionPacket::encode(snapshot->motion)) {
                m_motion.receive(sender, *motion);
                m_history.push_back(*snapshot);
                if (m_history.size() > SnapshotPlayback::kHistory) {
                    m_history.pop_front();
                }
            }
        }
    }
    return admitted;
}
std::optional<CombatSnapshot> CombatPlayback::sample(u64 tick, f32 fraction) const {
    const auto motion = m_motion.sample(tick, fraction);
    if (!motion || m_history.empty()) {
        return std::nullopt;
    }
    if (tick < m_history.front().motion.tick) {
        return m_history.front();
    }
    for (usize i = 1; i < m_history.size(); ++i) {
        const auto& upper = m_history[i];
        if (upper.motion.tick <= tick) {
            continue;
        }
        const auto& lower = m_history[i - 1];
        const f32 t = static_cast<f32>((static_cast<f64>(tick - lower.motion.tick) + fraction) /
                                       static_cast<f64>(upper.motion.tick - lower.motion.tick));
        auto shown = lower;
        shown.motion = *motion;
        if (shown.geometry && upper.geometry && shown.geometry->layout == upper.geometry->layout &&
            shown.geometry->objectCount == upper.geometry->objectCount) {
            shown.geometry->darken = std::lerp(shown.geometry->darken, upper.geometry->darken, t);
            for (auto& object : shown.geometry->objects) {
                const auto found = std::ranges::lower_bound(upper.geometry->objects, object.index,
                                                            {}, &SceneGeometry::Object::index);
                if (found != upper.geometry->objects.end() && found->index == object.index &&
                    found->continuity == object.continuity) {
                    object.local = placementBetween(object.local, found->local, t);
                    if (found->visible == object.visible) {
                        object.alpha = std::lerp(object.alpha, found->alpha, t);
                    }
                }
            }
        }
        for (usize seat = 0; seat < shown.players.size(); ++seat) {
            const auto& from = lower.players[seat];
            const auto& to = upper.players[seat];
            const auto& start = lower.motion.players[seat];
            const auto& end = upper.motion.players[seat];
            if (from && to && start && end && from->life == to->life &&
                start->grant == end->grant && start->continuity == end->continuity) {
                auto player = *from;
                player.animation = between(from->animation, to->animation, t);
                for (usize slot = 0; slot < player.companions.size(); ++slot) {
                    auto& current = player.companions[slot];
                    const auto& next = to->companions[slot];
                    if (!current || !next || current->form != next->form ||
                        current->animation.generation != next->animation.generation ||
                        current->animation.sequence != next->animation.sequence ||
                        next->animation.frame < current->animation.frame ||
                        glm::distance(Vec3{current->placement[3]}, Vec3{next->placement[3]}) > 32) {
                        continue;
                    }
                    current->placement = placementBetween(current->placement, next->placement, t);
                    current->animation = between(current->animation, next->animation, t);
                    current->textureClock = std::lerp(current->textureClock, next->textureClock, t);
                    current->alpha = std::lerp(current->alpha, next->alpha, t);
                }
                shown.players[seat] = player;
            }
        }
        // Discrete state and absent actors belong to the lower tick. Never show
        // a birth early, blend a recycled slot, or interpolate health through death.
        for (auto& enemy : shown.enemies) {
            const auto found = std::ranges::lower_bound(upper.enemies, enemy.instance, {},
                                                        &EnemyCombatState::instance);
            if (found == upper.enemies.end() || found->instance != enemy.instance ||
                found->life != enemy.life || found->kind != enemy.kind ||
                found->tier != enemy.tier || found->variant != enemy.variant) {
                continue;
            }
            enemy.position = glm::mix(enemy.position, found->position, t);
            enemy.yaw = std::remainder(
                enemy.yaw + std::remainder(found->yaw - enemy.yaw, kTwoPi) * t, kTwoPi);
            enemy.animation = between(enemy.animation, found->animation, t);
        }
        for (auto& shot : shown.projectiles) {
            const auto found =
                std::ranges::lower_bound(upper.projectiles, shot.key(), {}, &ProjectileState::key);
            if (found == upper.projectiles.end() || found->key() != shot.key() ||
                found->resource != shot.resource || found->continuity != shot.continuity ||
                found->flags != shot.flags ||
                glm::distance(Vec3{shot.placement[3]}, Vec3{found->placement[3]}) > 32) {
                continue;
            }
            shot.placement = placementBetween(shot.placement, found->placement, t);
            if (glm::dot(shot.direction, found->direction) > 0) {
                shot.direction = glm::mix(shot.direction, found->direction, t);
            }
            shot.age = std::lerp(shot.age, found->age, t);
            shot.radius = std::lerp(shot.radius, found->radius, t);
            shot.alpha = std::lerp(shot.alpha, found->alpha, t);
            shot.textureFrame = std::lerp(shot.textureFrame, found->textureFrame, t);
            shot.animation = between(shot.animation, found->animation, t);
        }
        for (auto& item : shown.pickups) {
            const auto found =
                std::ranges::lower_bound(upper.pickups, item.instance, {}, &PickupState::instance);
            if (found == upper.pickups.end() || found->instance != item.instance ||
                found->resource != item.resource || found->continuity != item.continuity ||
                glm::distance(Vec3{item.placement[3]}, Vec3{found->placement[3]}) > 32) {
                continue;
            }
            item.placement = placementBetween(item.placement, found->placement, t);
            item.alpha = std::lerp(item.alpha, found->alpha, t);
            item.textureFrame = std::lerp(item.textureFrame, found->textureFrame, t);
            item.animation = between(item.animation, found->animation, t);
        }
        for (auto& fixture : shown.fixtures) {
            const auto found =
                std::ranges::lower_bound(upper.fixtures, fixture.key(), {}, &FixtureState::key);
            if (found == upper.fixtures.end() || found->key() != fixture.key() ||
                found->resource != fixture.resource || found->continuity != fixture.continuity ||
                found->cameraFacing != fixture.cameraFacing ||
                glm::distance(Vec3{fixture.placement[3]}, Vec3{found->placement[3]}) > 32) {
                continue;
            }
            fixture.placement = placementBetween(fixture.placement, found->placement, t);
            fixture.alpha = std::lerp(fixture.alpha, found->alpha, t);
            fixture.textureClock = std::lerp(fixture.textureClock, found->textureClock, t);
            if (fixture.pose.generation == found->pose.generation) {
                if (fixture.meshSequence == found->meshSequence &&
                    found->meshFrame >= fixture.meshFrame) {
                    fixture.meshFrame = std::lerp(fixture.meshFrame, found->meshFrame, t);
                }
                if (fixture.textureSequence == found->textureSequence &&
                    found->textureFrame >= fixture.textureFrame) {
                    fixture.textureFrame = std::lerp(fixture.textureFrame, found->textureFrame, t);
                }
            }
            fixture.pose = between(fixture.pose, found->pose, t);
        }
        for (auto& mesh : shown.fighters) {
            // Attached billboards and meters were posed with the host camera.
            // Never turn them toward a cut the shared camera has not reached yet.
            if (lower.motion.cameraContinuity != upper.motion.cameraContinuity) {
                continue;
            }
            const auto found =
                std::ranges::lower_bound(upper.fighters, mesh.key(), {}, &FighterMeshState::key);
            if (found == upper.fighters.end() || found->key() != mesh.key() ||
                found->incarnation != mesh.incarnation || found->resource != mesh.resource ||
                found->nodes.size() != mesh.nodes.size() ||
                glm::distance(Vec3{mesh.placement[3]}, Vec3{found->placement[3]}) > 32) {
                continue;
            }
            mesh.placement = placementBetween(mesh.placement, found->placement, t);
            mesh.alpha = std::lerp(mesh.alpha, found->alpha, t);
            mesh.textureClock = std::lerp(mesh.textureClock, found->textureClock, t);
            const bool sameRoot =
                mesh.nodes.empty() || mesh.nodes[0].generation == found->nodes[0].generation;
            if (sameRoot && mesh.sequence == found->sequence && found->frame >= mesh.frame) {
                mesh.frame = std::lerp(mesh.frame, found->frame, t);
            }
            for (usize n = 0; n < mesh.nodes.size(); ++n) {
                auto& node = mesh.nodes[n];
                const auto& next = found->nodes[n];
                if (node.generation != next.generation || node.sequence != next.sequence ||
                    next.frame < node.frame) {
                    continue;
                }
                node.transform = placementBetween(node.transform, next.transform, t);
                node.frame = std::lerp(node.frame, next.frame, t);
                if (node.alpha > 0 && next.alpha > 0) {
                    node.alpha = std::lerp(node.alpha, next.alpha, t);
                }
            }
        }
        return shown;
    }
    return m_history.back();
}
} // namespace gdl::game
