#include "expansions/dreamcast/level/LegendsDreamcastWorldMotion.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <functional>
#include <map>
#include <utility>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"
#include "engine/math/RandomDistribution.h"
#include "engine/world/TreePose.h"

#include "game/level/ItemFigure.h"

namespace gdl::game {
namespace {
s16 parameter(const formats::ItemInstanceRecord& instance, usize at) {
    return std::bit_cast<s16>(readU16LE(instance.params, at));
}
u32 switchFlags(s32 subtype, u16 authored) {
    // SetItem8c0a8340, native type5 case. The upper byte remains authored;
    // subtype24 is intentionally the default, unlike subtype23's fixed latch.
    u32 kind = authored | 8U;
    switch (subtype) {
    case 20: kind = 0x10; break;
    case 21: kind = 8; break;
    case 22: kind = 0x12; break;
    case 23:
    case 29: kind = 10; break;
    case 25: kind = 0x804; break;
    case 26: kind = 2; break;
    case 27: kind = 0x80c; break;
    case 28: kind = 9; break;
    default: break;
    }
    return kind | (authored & 0xff00U);
}
bool finite(const Mat4& value) {
    return std::ranges::all_of(std::span<const f32, 16>{glm::value_ptr(value), 16},
                               [](f32 v) { return std::isfinite(v); });
}
} // namespace

LegendsDreamcastWorldMotion::LegendsDreamcastWorldMotion(
    const formats::LegendsMap& authored, const formats::LegendsMap& initial,
    const formats::LegendsSceneMotion& motion, const formats::LegendsDreamcastScene& scene,
    s32 players)
    : m_objects(motion.objects), m_parents(motion.parents) {
    const usize count = m_objects.size();
    if (count != authored.objects.size() || count != initial.objects.size() ||
        count != m_parents.size())
        throw FormatError("Dreamcast world motion has mismatched object identities");
    m_world.resize(count);
    m_previous.resize(count);
    m_moving.resize(count);
    for (usize i = 0; i < count; ++i) {
        const auto& first = initial.objects[i].transform;
        const auto& rest = authored.objects[i].transform;
        if (!finite(first) || !finite(rest) || glm::determinant(rest) == 0)
            throw FormatError("Dreamcast world motion has an invalid authored placement");
        // A permanently collapsed decoration has no inverse. Such an object
        // cannot serve as a moving floor; retain its baked empty presentation.
        m_initialInverse.push_back(glm::determinant(first) != 0 ? glm::inverse(first) : Mat4{0});
        m_world[i] = m_previous[i] = first;
    }
    for (const auto& definition : motion.worldAnimations) {
        const u32 flags = m_objects[static_cast<usize>(definition.objectIndex)].flags;
        Track track{definition, definition.startFrame};
        track.reverse = (flags & 0x100000U) != 0;
        track.once = (flags & 0x300000U) != 0;
        track.running = definition.frameCount > 1 && (flags & 0x300000U) != 0x300000U;
        m_tracks.push_back(std::move(track));
    }
    // InitWorld8c0a2b18..2c4c hides four named A5/F2 lightning nodes and
    // starts independent 5..15-second clocks. These are scripted flashes,
    // not texture or object animation tracks. Keep presentation randomness
    // separate from combat and generator streams.
    for (usize i = 0; i < count; ++i) {
        const auto& name = m_objects[i].name;
        for (const auto prefix : {"A5XPXLNC_LNING", "F2XPNC_LNING"}) {
            if (name.size() == std::string_view{prefix}.size() + 1 && name.starts_with(prefix) &&
                name.back() >= '1' && name.back() <= '4') {
                m_lightning.push_back({i,
                                       5.f + static_cast<f32>(*random_distribution::boundedIndex(
                                                 m_lightningRandom, 0x1000000)) *
                                                 (10.f / 0x1000000),
                                       false});
            }
        }
    }
    for (usize index = 0; index < scene.itemInstances.size(); ++index) {
        const auto& instance = scene.itemInstances[index];
        if (instance.info < 0 || !shownToParty(instance.minPlayers, players))
            continue;
        const auto& info = scene.itemInfos.at(static_cast<usize>(instance.info));
        if (info.type != 5)
            continue;
        Switch pad;
        pad.nativeIndex = static_cast<u32>(index);
        pad.target = parameter(instance, 0);
        if (pad.target < 0 || static_cast<usize>(pad.target) >= count) {
            m_warnings.push_back(std::format("Dreamcast switch {} has no world target", index));
            pad.target = -1;
        }
        pad.flags = switchFlags(info.subtype, static_cast<u16>(parameter(instance, 2)));
        pad.id = instance.params[6];
        pad.nextId = instance.params[7];
        pad.name = instance.name.empty() ? info.name : instance.name;
        pad.hidden = (instance.flags & 2U) != 0;
        pad.position = instance.position;
        pad.rotation = instance.rotation;
        pad.radius = instance.params[4] == 255 ? .01f : .5f * instance.params[4];
        if (pad.radius == 0)
            pad.radius = info.radius;
        pad.height = info.height;
        m_switches.push_back(pad);
        if (pad.target < 0)
            continue;
        auto* target = targetOf(pad.target);
        if (!target) {
            Target created;
            created.object = pad.target;
            created.subtype = info.subtype;
            created.soundSlot = std::bit_cast<s8>(instance.params[5]);
            created.flags = pad.flags;
            created.closed = .1f * parameter(instance, 8);
            created.opened = .1f * parameter(instance, 10);
            created.height = created.closed;
            created.alpha = created.previousAlpha = (pad.flags & 0x10U) != 0 ? 0.f : 1.f;
            for (auto& track : m_tracks) {
                if (track.definition.objectIndex == pad.target) {
                    created.animated = track.definition.frameCount > 1;
                    // ProcessItemWobjs starts a registered, closed keyed
                    // target in reverse until its authored cursor reaches0.
                    track.reverse = true;
                    track.once = true;
                    track.running = created.animated && track.frame > 0;
                    created.moving |= track.running;
                }
            }
            m_targets.push_back(created);
        } else {
            if (target->soundSlot == 0)
                target->soundSlot = std::bit_cast<s8>(instance.params[5]);
            if (target->closed == 0)
                target->height = target->closed = .1f * parameter(instance, 8);
            if (target->opened == 0)
                target->opened = .1f * parameter(instance, 10);
        }
    }
    for (auto& pad : m_switches) {
        if (pad.nextId != 0) {
            for (usize i = 0; i < m_switches.size(); ++i) {
                if (&m_switches[i] != &pad && m_switches[i].id == pad.nextId &&
                    (m_switches[i].flags & 0x40U) == 0) {
                    pad.next = static_cast<s32>(i);
                    break;
                }
            }
        }
    }
    for (usize i = 0; i < count; ++i) {
        const auto index = static_cast<s32>(i);
        m_moving[i] =
            std::ranges::any_of(m_tracks,
                                [&](const auto& track) {
                                    return track.definition.frameCount > 1 &&
                                           descendant(index, track.definition.objectIndex);
                                }) ||
            std::ranges::any_of(
                m_targets, [&](const auto& target) { return descendant(index, target.object); });
        if (m_moving[i] && (m_objects[i].flags & 0x01001000U) != 0)
            m_collisionOwners.push_back(index);
    }
    compose();
    // InitItems8c0a9b20 runs ProcessItemWobjs before its AddItemSub pass.
    // Both FloorPos and UnparentMatrix therefore see the initialized target
    // pose, not the authored rest matrix or the renderer's baked first key.
    std::vector<CollisionTriangle> floors;
    for (const auto& triangle : authored.collision) {
        if (!triangle.floor)
            continue;
        CollisionTriangle floor{triangle.normal, triangle.vertices, triangle.owner, 4};
        const auto owner = static_cast<usize>(triangle.owner);
        if (triangle.owner >= 0 && owner < count && (m_objects[owner].flags & 0x01001000U) != 0) {
            const Mat4 change = m_world[owner] * glm::inverse(authored.objects[owner].transform);
            if (glm::determinant(change) == 0)
                continue;
            for (auto& vertex : floor.vertices)
                vertex = Vec3{change * Vec4{vertex, 1}};
            floor.normal =
                glm::normalize(glm::transpose(glm::inverse(Mat3{change})) * floor.normal);
            if (floor.normal.y < WorldCollision::kFloorNormalY)
                continue;
        }
        floors.push_back(floor);
    }
    m_bindingFloors.build(std::move(floors), m_parents);
    for (const auto& placement : m_world)
        m_bindingInverse.push_back(glm::determinant(placement) != 0 ? glm::inverse(placement)
                                                                    : Mat4{0});
    for (auto& pad : m_switches) {
        const auto& instance = scene.itemInstances[pad.nativeIndex];
        const auto& info = scene.itemInfos[static_cast<usize>(instance.info)];
        if ((info.collisionFlags & 1) == 0) {
            if (const auto floor = supportAt(instance.position);
                floor && carriesItems(floor->object))
                pad.floor = floor->object;
            pad.position = itemPosition(info, instance.position);
        }
    }
    capturePresentation();
}

bool LegendsDreamcastWorldMotion::descendant(s32 object, s32 ancestor) const {
    for (usize depth = 0;
         object >= 0 && static_cast<usize>(object) < m_parents.size() && depth < m_parents.size();
         ++depth) {
        if (object == ancestor)
            return true;
        object = m_parents[static_cast<usize>(object)];
    }
    return false;
}
LegendsDreamcastWorldMotion::Target* LegendsDreamcastWorldMotion::targetOf(s32 object) {
    const auto at = std::ranges::find(m_targets, object, &Target::object);
    return at == m_targets.end() ? nullptr : &*at;
}
void LegendsDreamcastWorldMotion::capturePresentation() {
    m_previous = m_world;
    for (auto& target : m_targets)
        target.previousAlpha = target.alpha;
}
void LegendsDreamcastWorldMotion::compose() {
    std::vector<NodePose> poses(m_objects.size());
    for (const auto& track : m_tracks) {
        const auto& definition = track.definition;
        if ((m_objects[static_cast<usize>(definition.objectIndex)].flags & 0x300000U) ==
                0x300000U &&
            !std::ranges::any_of(m_targets, [&](const auto& target) {
                return target.object == definition.objectIndex;
            }))
            continue;
        const TrackInfo keys{definition.track.node, definition.track.flags, definition.track.frames,
                             definition.track.values};
        auto& pose = poses[static_cast<usize>(definition.objectIndex)];
        const auto sampled = TreePose::sample(keys, track.frame);
        if ((keys.flags & 7U) != 0) {
            pose.rotation = sampled.rotation;
            pose.rotationOrder = sampled.rotationOrder;
        }
        if ((keys.flags & 0x70U) != 0)
            pose.position = sampled.position;
        if ((keys.flags & 0x700U) != 0)
            pose.scale = sampled.scale;
    }
    for (const auto& target : m_targets) {
        if (!target.animated && (target.flags & 0x10U) == 0)
            poses[static_cast<usize>(target.object)].position.y = target.height;
    }
    std::vector<u8> ready(m_objects.size());
    const std::function<void(usize)> build = [&](usize index) {
        if (ready[index] == 2)
            return;
        if (ready[index] == 1)
            throw FormatError("Dreamcast animated world has cyclic parents");
        ready[index] = 1;
        m_world[index] = TreePose::localMatrix(poses[index], m_objects[index].position);
        const s32 parent = m_parents[index];
        if (parent >= 0) {
            build(static_cast<usize>(parent));
            m_world[index] = m_world[static_cast<usize>(parent)] * m_world[index];
        }
        ready[index] = 2;
    };
    for (usize i = 0; i < m_objects.size(); ++i)
        build(i);
}
void LegendsDreamcastWorldMotion::fire(Target& target, bool open) {
    if (target.open == open)
        return;
    target.open = open;
    target.moving = true;
    for (auto& track : m_tracks) {
        if (track.definition.objectIndex == target.object && track.definition.frameCount > 1) {
            track.running = true;
            track.once = (target.flags & 0x20U) == 0 || !open;
            track.reverse = !open;
        }
    }
}
void LegendsDreamcastWorldMotion::advance(f32 seconds) {
    if (!std::isfinite(seconds) || seconds <= 0 || seconds > 10)
        return;
    // UpdateWorld8c0a2fc0..3052 unhides for .2 seconds, then waits another
    // 5+frand(10). This world effect continues under the Stop Time item.
    for (auto& flash : m_lightning) {
        flash.remaining -= seconds;
        while (flash.remaining <= 0) {
            flash.visible = !flash.visible;
            flash.remaining += flash.visible
                                   ? .2f
                                   : 5.f + static_cast<f32>(*random_distribution::boundedIndex(
                                               m_lightningRandom, 0x1000000)) *
                                               (10.f / 0x1000000);
        }
    }
    for (auto& pad : m_switches)
        pad.cooldown = std::max(0.f, pad.cooldown - seconds);
    std::map<u32, std::pair<u32, s32>> sounding;
    for (const auto& target : m_targets) {
        if (target.moving && target.sounds)
            sounding.try_emplace((*target.sounds)[0], (*target.sounds)[1], target.object);
    }
    // DoWorldAnimSub8c0e9700 advances native keys at30Hz, independently of
    // the item/action timer's60Hz clock. Retain the authored starting cursor.
    for (auto& track : m_tracks) {
        if (!track.running)
            continue;
        const f32 last = static_cast<f32>(track.definition.frameCount - 1);
        track.frame += seconds * (track.reverse ? -30.f : 30.f);
        if (track.frame >= last || track.frame <= 0) {
            if (track.once) {
                track.frame = std::clamp(track.frame, 0.f, last);
                track.running = false;
            } else if (last > 0) {
                track.frame = std::fmod(track.frame, last);
                if (track.frame < 0)
                    track.frame += last;
            }
        }
    }
    for (auto& target : m_targets) {
        if (target.animated) {
            target.moving = std::ranges::any_of(m_tracks, [&](const auto& track) {
                return track.definition.objectIndex == target.object && track.running;
            });
        } else if ((target.flags & 0x10U) != 0) {
            const f32 wanted = target.open ? 1.f : 0.f;
            const f32 step = seconds * 30.f * 16.f / 255.f;
            target.alpha += std::clamp(wanted - target.alpha, -step, step);
            target.moving = target.alpha != wanted;
        } else {
            const f32 wanted = target.open ? target.opened : target.closed;
            // ProcessItemWobjs8c0ab44e clamps the remaining distance by
            // four units per second, with a .001-unit completion deadband.
            const f32 distance = wanted - target.height;
            target.moving = std::abs(distance) > .001f;
            if (target.moving) {
                const f32 step = seconds * 4.f;
                target.height += std::clamp(distance, -step, step);
            } else if ((target.flags & 0x20U) != 0) {
                target.open = !target.open;
                target.moving = true;
            }
        }
    }
    compose();
    for (const auto& [token, stopped] : sounding) {
        if (std::ranges::none_of(m_targets, [&](const auto& target) {
                return target.moving && target.sounds && (*target.sounds)[0] == token;
            }))
            m_soundEvents.push_back(
                {stopped.first, Vec3{m_world[static_cast<usize>(stopped.second)][3]}});
    }
}
void LegendsDreamcastWorldMotion::support(std::span<const s32> owners) {
    m_support.assign(owners.begin(), owners.end());
}
void LegendsDreamcastWorldMotion::touch(std::span<const LegendsPortalVisitor> visitors) {
    u8 living = 0;
    std::vector<u8> contacts(m_switches.size());
    for (const auto& visitor : visitors) {
        if (visitor.alive && visitor.player < 4)
            living |= static_cast<u8>(1U << visitor.player);
    }
    for (usize i = 0; i < m_switches.size(); ++i) {
        const auto& pad = m_switches[i];
        const Vec3 spot = switchPosition(i);
        for (const auto& visitor : visitors) {
            if (!visitor.alive || visitor.player >= 4 ||
                ((pad.flags & 0x100U) != 0 && !descendant(visitor.floorObject, pad.target)))
                continue;
            const Vec3 away = visitor.position - spot;
            const f32 radius = visitor.radius + pad.radius;
            const f32 rise = away.y + visitor.height * .5f - 1.f;
            if (away.x * away.x + away.z * away.z > radius * radius ||
                std::abs(rise) > pad.height + visitor.height * .5f)
                continue;
            s32 next = static_cast<s32>(i);
            for (usize depth = 0; next >= 0 && depth < m_switches.size(); ++depth) {
                contacts[static_cast<usize>(next)] |= static_cast<u8>(1U << visitor.player);
                next = m_switches[static_cast<usize>(next)].next;
            }
        }
    }
    for (auto& target : m_targets)
        target.pressed = false;
    for (usize i = 0; i < m_switches.size(); ++i) {
        auto& pad = m_switches[i];
        // Each chained node retains its own whole-party admission. Requirement
        // pads need the platform's progression adapter; never bypass a lock.
        const bool contact = contacts[i] != 0 && (pad.flags & 0x40U) == 0 &&
                             ((pad.flags & 0x400U) == 0 || contacts[i] == living);
        auto* target = targetOf(pad.target);
        if (!target) {
            pad.active = pad.active || contact;
            continue;
        }
        target->pressed |= contact;
        bool open = target->open;
        if ((pad.flags & 1U) != 0) {
            if (contact && !target->moving)
                open = false;
            pad.active = !open;
        } else if ((pad.flags & 2U) != 0) {
            if (contact && !target->moving)
                open = true;
            pad.active = open;
        } else if ((pad.flags & 4U) != 0) {
            if (contact && !target->moving && pad.cooldown == 0) {
                open = !open;
                pad.cooldown = 2;
            }
            pad.active = contact;
        } else {
            open |= contact;
            pad.active = contact;
        }
        fire(*target, open);
    }
    for (auto& target : m_targets) {
        if ((target.flags & 7U) != 0 || target.pressed)
            continue;
        const bool supported =
            std::ranges::any_of(visitors,
                                [&](const auto& visitor) {
                                    return visitor.alive &&
                                           descendant(visitor.floorObject, target.object);
                                }) ||
            std::ranges::any_of(m_support,
                                [&](s32 owner) { return descendant(owner, target.object); });
        if (!supported)
            fire(target, false);
    }
}
Mat4 LegendsDreamcastWorldMotion::transform(usize object, f32 alpha) const {
    if (object >= m_world.size() || m_initialInverse[object] == Mat4{0})
        return Mat4{1};
    Mat4 placed = m_world[object];
    if (alpha >= 0) {
        const f32 t = std::clamp(alpha, 0.f, 1.f);
        for (Mat4::length_type i = 0; i < 4; ++i)
            placed[i] = glm::mix(m_previous[object][i], placed[i], t);
    }
    return placed * m_initialInverse[object];
}
Mat4 LegendsDreamcastWorldMotion::riderTransform(s32 owner) const {
    if (owner < 0 || static_cast<usize>(owner) >= m_world.size() ||
        m_bindingInverse[static_cast<usize>(owner)] == Mat4{0})
        return Mat4{1};
    return m_world[static_cast<usize>(owner)] * m_bindingInverse[static_cast<usize>(owner)];
}

std::optional<std::pair<usize, Vec3>> LegendsDreamcastWorldMotion::ambientParent(Vec3 position,
                                                                                 f32 radius) const {
    std::optional<std::pair<usize, Vec3>> result;
    if (!std::isfinite(radius) || radius <= 0)
        return result;
    for (const auto& track : m_tracks) {
        const auto object = track.definition.objectIndex;
        if (object < 0 || static_cast<usize>(object) >= m_world.size())
            continue;
        const Vec3 origin{m_world[static_cast<usize>(object)][3]};
        const f32 distance = glm::distance(position, origin);
        if (std::isfinite(distance) && distance < radius) {
            radius = distance;
            result = {{static_cast<usize>(object), origin}};
        }
    }
    return result;
}
Vec3 LegendsDreamcastWorldMotion::offset(usize object, f32 alpha) const {
    if (object >= m_world.size() || m_initialInverse[object] == Mat4{0})
        return Vec3{0};
    const Mat4 initial = glm::inverse(m_initialInverse[object]);
    return Vec3{transform(object, alpha) * initial[3] - initial[3]};
}
f32 LegendsDreamcastWorldMotion::opacity(usize object, f32 alpha) const {
    for (const auto& flash : m_lightning) {
        if (flash.object == object && !flash.visible)
            return 0;
    }
    f32 result = 1;
    for (const auto& target : m_targets) {
        if (descendant(static_cast<s32>(object), target.object))
            result *= alpha >= 0 ? std::lerp(target.previousAlpha, target.alpha,
                                             std::clamp(alpha, 0.f, 1.f))
                                 : target.alpha;
    }
    return result;
}
bool LegendsDreamcastWorldMotion::moves(s32 object) const {
    return object >= 0 && static_cast<usize>(object) < m_moving.size() &&
           m_moving[static_cast<usize>(object)];
}
bool LegendsDreamcastWorldMotion::activatesBoss(s32 support) const {
    if (support < 0 || static_cast<usize>(support) >= m_parents.size())
        return false;
    const s32 parent = m_parents[static_cast<usize>(support)];
    return std::ranges::any_of(m_targets, [&](const auto& target) {
        // ProcessItemWobjs8c0ab4e0 sets0x10 while moving; its final low-mode
        // gate clears transient state for unlatched low0 controls. A5 subtype26
        // supplies low2 (authored0x400 becomes0x402), so its flag is retained.
        return (target.object == support || target.object == parent) && target.moving &&
               (target.flags & 7U) != 0;
    });
}
std::optional<FloorHit> LegendsDreamcastWorldMotion::supportAt(Vec3 position) const {
    return m_bindingFloors.restingFloorAt(position, ItemFigure::kFloorReachAbove,
                                          ItemFigure::kFloorReachBelow,
                                          ItemFigure::kFloorReachRadius);
}
Vec3 LegendsDreamcastWorldMotion::itemPosition(const formats::ItemInfoRecord& info,
                                               Vec3 authored) const {
    // AddItemSub8c0a99e0 skips only collision flag1. FloorPos8c0eacc0
    // preserves the supplied Y when no floor is found; the caller still
    // adds its .1-unit lift. Do not preserve an arbitrary authored air gap.
    if ((info.collisionFlags & 1) == 0) {
        if (const auto floor = supportAt(authored))
            authored.y = floor->y;
        authored.y += ItemFigure::kFloorLift;
    }
    return authored;
}
bool LegendsDreamcastWorldMotion::carriesItems(s32 object) const {
    return object >= 0 && static_cast<usize>(object) < m_objects.size() &&
           (m_objects[static_cast<usize>(object)].flags & 0x1000U) != 0 &&
           m_bindingInverse[static_cast<usize>(object)] != Mat4{0};
}
Vec3 LegendsDreamcastWorldMotion::switchPosition(usize index) const {
    const auto& pad = m_switches.at(index);
    return Vec3{riderTransform(pad.floor) * Vec4{pad.position, 1}};
}
void LegendsDreamcastWorldMotion::bindCollision(WorldCollision& collision) const {
    collision.setMovingObjects(m_collisionOwners, 0x1000);
    syncCollision(collision);
}
void LegendsDreamcastWorldMotion::syncCollision(WorldCollision& collision) const {
    for (s32 owner : m_collisionOwners) {
        collision.setObjectTransform(owner, transform(static_cast<usize>(owner)));
        bool solid = true;
        u8 side = 0;
        for (const auto& target : m_targets) {
            if (!descendant(owner, target.object))
                continue;
            if ((target.flags & 0x10U) != 0)
                solid &= target.alpha == 1;
            else if ((target.flags & 8U) == 0)
                side |= target.moving ? 1 : 0;
        }
        // ProcessItemWobjs writes WorldObj.side=1 during movement, not a
        // global collision disable. ProcessCritter's floor category2 still
        // follows A5's descending platform while player category1 skips it.
        collision.setCollisionSide(owner, side);
        collision.setSolid(owner, solid);
    }
}

void LegendsDreamcastWorldMotion::bindSounds(const SoundResolver& resolve) {
    for (auto& target : m_targets) {
        // ProcessItemWobjs8c0ab040 routes bridge subtypes separately and
        // reserves slots>=10 for one-shot sounds. Only verified elevator
        // start/stop pairs enter the movement-loop lane.
        if (target.soundSlot >= 0 && target.soundSlot < 10 && target.subtype != 20 &&
            target.subtype != 22)
            target.sounds = resolve(target.soundSlot);
    }
}

std::vector<LegendsSoundLoop> LegendsDreamcastWorldMotion::soundLoops() const {
    std::vector<LegendsSoundLoop> result;
    for (const auto& target : m_targets) {
        // AudioIsPlaying deduplicates the authored start token, even when
        // several pads move different objects using the same movement sound.
        if (target.moving && target.sounds && std::ranges::none_of(result, [&](const auto& loop) {
                return loop.token == (*target.sounds)[0];
            }))
            result.push_back({0x20000000U + (*target.sounds)[0], (*target.sounds)[0],
                              Vec3{m_world[static_cast<usize>(target.object)][3]}});
    }
    return result;
}

std::vector<LegendsSoundEvent> LegendsDreamcastWorldMotion::takeSounds() {
    return std::exchange(m_soundEvents, {});
}

} // namespace gdl::game
