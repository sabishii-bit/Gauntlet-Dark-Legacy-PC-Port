#include "expansions/n64/level/LegendsWorldMotion.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <stdexcept>
#include <utility>

#include "expansions/n64/formats/LegendsN64Scene.h"

namespace gdl::game {

LegendsWorldMotion::LegendsWorldMotion(const formats::LegendsMap& map,
                                       std::span<const formats::LegendsN64Switch> definitions,
                                       u8 activationThreshold) {
    if (map.objects.size() > static_cast<usize>(std::numeric_limits<s32>::max())) {
        throw std::invalid_argument("Native motion object roster exceeds identity range");
    }
    for (const auto& object : map.objects) {
        m_objectParents.push_back(object.parent);
    }
    for (const s32 first : m_objectParents) {
        usize depth = 0;
        for (s32 parent = first; parent != -1;
             parent = m_objectParents[static_cast<usize>(parent)]) {
            if (parent < 0 || static_cast<usize>(parent) >= map.objects.size() ||
                ++depth >= map.objects.size()) {
                throw std::invalid_argument("Invalid native motion object hierarchy");
            }
        }
    }
    const auto findTarget = [&](std::string_view name) -> s32 {
        const auto match = [&](bool exact) -> s32 {
            s32 chosen = -1;
            for (usize i = 0; i < map.objects.size(); ++i) {
                const auto& object = map.objects[i];
                bool matches = object.name == name;
                if (!exact && object.name.starts_with(name) && object.name.size() > name.size()) {
                    const char boundary = object.name[name.size()];
                    matches = boundary == '-' || boundary == '#' || boundary == '_';
                }
                if (matches &&
                    (chosen < 0 ||
                     object.nativeOrder < map.objects[static_cast<usize>(chosen)].nativeOrder)) {
                    chosen = static_cast<s32>(i);
                }
            }
            return chosen;
        };
        // e003aa6c searches exact names first, then e003ab9c permits only
        // authored separator suffixes. Both walk node, child, then sibling.
        const s32 exact = match(true);
        return exact >= 0 ? exact : match(false);
    };
    std::vector<formats::LegendsN64Switch> bound;
    for (const auto& definition : definitions) {
        if (!definition.supported || definition.minimumPlayers > activationThreshold) {
            continue;
        }
        const s32 object = findTarget(definition.target);
        if (object < 0) {
            m_warnings.push_back(std::format("Native switch target {} has no world placement; "
                                             "motion is not bound.",
                                             definition.target));
            continue;
        }
        auto projected = definition;
        projected.target = map.objects[static_cast<usize>(object)].name;
        formats::LegendsN64Fixture fixture;
        fixture.kind = formats::LegendsN64FixtureKind::Trap;
        fixture.position = definition.position;
        fixture.groundProjected = definition.groundProjected;
        const auto placement = formats::legendsN64FixturePlacement(fixture, map.collision);
        projected.position = placement.position;
        if (placement.floorObject >= 0) {
            // e00694e4 stores the supporting foot Y as a signed fixed-point
            // value. e0064838 restores it and reapplies the shared .2 pad.
            projected.position.y = std::trunc((projected.position.y - .2f) * 64) / 64 + .2f;
        }
        m_plateOwners.push_back(placement.floorObject);
        bound.push_back(std::move(projected));
    }
    m_motion = LegendsSwitches{bound, activationThreshold};
    m_objectTargets.assign(map.objects.size(), -1);
    for (usize target = 0; target < m_motion.targets().size(); ++target) {
        const auto index = findTarget(m_motion.targets()[target].name);
        if (m_motion.targets()[target].fading) {
            m_fadingObjects.push_back(index);
        } else if ((map.objects[static_cast<usize>(index)].flags & 0x8000U) != 0) {
            m_collisionObjects.push_back(index);
        }
        m_objectTargets[static_cast<usize>(index)] = static_cast<s32>(target);
        m_soundPositions.emplace_back(map.objects[static_cast<usize>(index)].transform[3]);
        m_sounding.push_back(false);
    }
}

void LegendsWorldMotion::update(f32 seconds) {
    if (!std::isfinite(seconds) || seconds <= 0 || seconds > 10) {
        return;
    }
    std::vector<f32> before;
    before.reserve(m_motion.targets().size());
    for (const auto& target : m_motion.targets()) {
        before.push_back(target.offset);
    }
    m_motion.update(seconds);
    for (usize i = 0; i < m_motion.targets().size(); ++i) {
        const auto& target = m_motion.targets()[i];
        if (target.fading || target.offset == before[i]) {
            continue;
        }
        // game.bin e006e5dc retains cue1fe while physical displacement
        // advances, then stops its handle and plays1ff exactly at the goal.
        // Contact alone (or a zero-distance request) produces neither cue.
        m_sounding[i] = target.moving;
        if (!target.moving) {
            m_soundEvents.push_back({0x1ff, m_soundPositions[i] + Vec3{0, target.offset, 0}});
        }
    }
}

std::vector<LegendsSoundLoop> LegendsWorldMotion::soundLoops() const {
    std::vector<LegendsSoundLoop> result;
    for (usize i = 0; i < m_sounding.size(); ++i) {
        if (m_sounding[i]) {
            result.push_back({static_cast<u32>(i), 0x1fe,
                              m_soundPositions[i] + Vec3{0, m_motion.targets()[i].offset, 0}});
        }
    }
    return result;
}

std::vector<LegendsSoundEvent> LegendsWorldMotion::takeSounds() {
    return std::exchange(m_soundEvents, {});
}

void LegendsWorldMotion::touch(std::span<const LegendsPortalVisitor> visitors) {
    std::vector<usize> supported;
    const auto addSupport = [&](s32 object) {
        if (object >= 0 && static_cast<usize>(object) < m_objectTargets.size() &&
            m_objectTargets[static_cast<usize>(object)] >= 0) {
            supported.push_back(static_cast<usize>(m_objectTargets[static_cast<usize>(object)]));
        }
    };
    for (const auto& visitor : visitors) {
        if (visitor.alive && visitor.player < 4) {
            addSupport(visitor.floorObject);
        }
    }
    for (const s32 owner : m_supportingObjects) {
        addSupport(owner);
    }
    m_motion.support(supported);
    for (const auto& plate : m_motion.switches()) {
        auto definition = plate.definition;
        definition.position = *platePosition(definition.nativeIndex);
        u8 occupants = 0;
        for (const auto& visitor : visitors) {
            if (visitor.player < 4 && touchesLegendsSwitch(definition, visitor)) {
                occupants |= static_cast<u8>(1U << visitor.player);
            }
        }
        m_motion.contact(plate.definition.nativeIndex, occupants);
    }
}

void LegendsWorldMotion::support(std::span<const s32> owners) {
    m_supportingObjects.assign(owners.begin(), owners.end());
}

std::optional<Vec3> LegendsWorldMotion::platePosition(u32 nativeIndex) const {
    const auto plates = m_motion.switches();
    const auto found = std::ranges::find_if(
        plates, [&](const auto& plate) { return plate.definition.nativeIndex == nativeIndex; });
    if (found == plates.end()) {
        return std::nullopt;
    }
    const auto index = static_cast<usize>(found - plates.begin());
    // e00694e4 stores the selected floor's WOBJ owner and pre-motion footY.
    // e0064838 carries its contact and artwork by that owner's motion only.
    return found->definition.position + Vec3{0, targetOffset(m_plateOwners[index], -1), 0};
}

bool LegendsWorldMotion::activated(u32 nativeIndex) const {
    const auto found = std::ranges::find_if(m_motion.switches(), [&](const auto& plate) {
        return plate.definition.nativeIndex == nativeIndex;
    });
    return found != m_motion.switches().end() && found->activated;
}

void LegendsWorldMotion::bindCollision(WorldCollision& collision) const {
    // 0x1000 is the common engine's moving-floor semantic used by FloorRiding
    // and enemy support, not a copy of native Legends object flag bits.
    if (!m_collisionObjects.empty()) {
        collision.setMovingObjects(m_collisionObjects, 0x1000);
    }
    // Fade records have no displacement, but their native WOBJ8000 owner still
    // retains supported players/enemies. Keep an identity dynamic transform so
    // Enemies::rememberFloor can retain that owner without a second floor query.
    if (!m_fadingObjects.empty()) {
        collision.setMovingObjects(m_fadingObjects, 0x1000);
    }
    syncCollision(collision);
}

void LegendsWorldMotion::syncCollision(WorldCollision& collision) const {
    for (const s32 object : m_collisionObjects) {
        collision.setObjectTransform(object,
                                     glm::translate(Mat4{1}, Vec3{0, collisionOffset(object), 0}));
    }
    for (const s32 object : m_fadingObjects) {
        const auto& target =
            m_motion.targets()[static_cast<usize>(m_objectTargets[static_cast<usize>(object)])];
        // e006e5dc clears the collision-exclusion bits only at the fully
        // revealed endpoint. Fading is not a displacement of the triangles.
        collision.setSolid(object, target.offset == target.second);
    }
}

f32 LegendsWorldMotion::opacity(usize object, f32 alpha) const {
    if (object >= m_objectTargets.size()) {
        return 1;
    }
    f32 opacity = 1;
    for (s32 current = static_cast<s32>(object); current >= 0;
         current = m_objectParents[static_cast<usize>(current)]) {
        const s32 index = m_objectTargets[static_cast<usize>(current)];
        if (index >= 0) {
            const auto& target = m_motion.targets()[static_cast<usize>(index)];
            if (target.fading) {
                // e00281f8 recursively writes the native transparency byte.
                opacity =
                    std::min(opacity, 1.0f - std::clamp(target.sample(alpha) / 1.5f, 0.0f, 1.0f));
            }
        }
    }
    return opacity;
}

Vec3 LegendsWorldMotion::offset(usize object, f32 alpha) const {
    if (object >= m_objectTargets.size()) {
        return Vec3{0};
    }
    // WorldLoad e003955c halves native draw translations and vertices, and
    // e002bfe8 expands matrix translation by64 for the RSP. Our reader unfolds
    // those units into full authored MTRX + Vtx/64. Consequently the native
    // drawable offset/2 in e006e5dc becomes the full collision offset here.
    f32 y = 0;
    for (s32 current = static_cast<s32>(object); current >= 0;
         current = m_objectParents[static_cast<usize>(current)]) {
        y += targetOffset(current, alpha);
    }
    return {0, y, 0};
}

f32 LegendsWorldMotion::collisionOffset(s32 object, f32 alpha) const {
    // e003faf4 applies a motion record only to a collision owner with8000;
    // descendants and decorative drawables do not inherit collision movement.
    return std::ranges::find(m_collisionObjects, object) != m_collisionObjects.end()
               ? targetOffset(object, alpha)
               : 0;
}

f32 LegendsWorldMotion::targetOffset(s32 object, f32 alpha) const {
    if (object < 0 || static_cast<usize>(object) >= m_objectTargets.size() ||
        m_objectTargets[static_cast<usize>(object)] < 0) {
        return 0;
    }
    const auto& target =
        m_motion.targets()[static_cast<usize>(m_objectTargets[static_cast<usize>(object)])];
    return target.fading ? 0 : target.sample(alpha);
}

} // namespace gdl::game
