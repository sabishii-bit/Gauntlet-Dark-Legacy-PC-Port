#include "game/world/LevelTriggers.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "engine/core/Types.h"

namespace gdl::game {

namespace {

constexpr u32 kDefaultFlags = 0x8;
constexpr u8 kTinyRadius = 0xFF;
constexpr f32 kToggleDelay = 2.0f;
constexpr f32 kHeightSpeed = 4.0f;
constexpr f32 kHeightTolerance = 0.001f;

s16 paramS16(const ItemInstance& instance, usize at) {
    s16 value = 0;
    std::memcpy(&value, &instance.params[at], sizeof(value));
    return value;
}

} // namespace

s32 LevelTriggers::crystalsNeeded(s32 realm) {
    if (realm < 0 || static_cast<usize>(realm) >= kCrystalsToOpen.size()) {
        return 0;
    }
    return kCrystalsToOpen[static_cast<usize>(realm)];
}

void LevelTriggers::bind(const WorldLayout& layout, WorldAnimator& animator,
                         WorldCollision* collision) {
    clear();
    for (const auto& object : layout.objects()) {
        m_parents.push_back(object.parent);
    }
    const std::vector<ItemInfo>& infos = layout.itemInfos();
    const std::vector<ItemInstance>& instances = layout.itemInstances();
    for (usize i = 0; i < instances.size(); ++i) {
        const ItemInstance& instance = instances[i];
        if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size() ||
            infos[static_cast<usize>(instance.info)].type != ItemInfo::kTrigger) {
            continue;
        }
        const ItemInfo& info = infos[static_cast<usize>(instance.info)];
        LevelTrigger trigger;
        trigger.instance = static_cast<s32>(i);
        trigger.spot = instance.position;
        const s16 object = paramS16(instance, 0);
        trigger.target =
            object >= 0 && static_cast<usize>(object) < layout.objects().size() ? object : -1;
        // The trigger's flags: the kind's own, then whatever the instance adds.
        const u32 params = static_cast<u16>(paramS16(instance, 2));
        u32 flags = params | kDefaultFlags;
        switch (info.subtype) {
        case 20: flags = 0x10; break;
        case 21: flags = 8; break;
        case 22: flags = 0x12; break;
        case 23: flags = 10; break;
        case 25: flags = 0x804; break;
        case 26: flags = 2; break;
        case 27: flags = 0x80C; break;
        case 28: flags = 9; break;
        case 29: flags = 10; break;
        default: break;
        }
        flags |= params & ~0xFFU;
        trigger.flags = flags;
        trigger.kind = flags & 0xFFU;
        trigger.radius =
            instance.params[4] == kTinyRadius ? 0.01f : 0.5f * static_cast<f32>(instance.params[4]);
        if (trigger.radius <= 0.0f) {
            trigger.radius = info.radius; // the kind's own, when the instance gives none
        }
        trigger.id = instance.params[6];
        trigger.nextId = instance.params[7];
        // The slot is a signed byte in the data: 255 (and anything high) means none.
        const u8 slot = instance.params[5];
        trigger.sound = slot >= 0x80 ? -1 : static_cast<s32>(slot);
        m_triggers.push_back(trigger);
        if (trigger.target >= 0 && targetOf(trigger.target) == nullptr) {
            Target target;
            target.object = trigger.target;
            for (usize node = 0; node < m_parents.size(); ++node) {
                auto at = static_cast<s32>(node);
                for (usize guard = 0; at >= 0 && guard < m_parents.size(); ++guard) {
                    if (at == target.object) {
                        target.subtree.push_back(node);
                        break;
                    }
                    at = m_parents[static_cast<usize>(at)];
                }
            }
            target.kind = trigger.kind;
            target.origin = layout.objects()[static_cast<usize>(trigger.target)].position;
            // RegisterItemWobj stores the two signed endpoint parameters in tenths.
            // Without a keyed track, ProcessItemWobjs translates the node vertically.
            target.height = 0.1f * static_cast<f32>(paramS16(instance, 8));
            target.closedHeight = target.height;
            target.openHeight = 0.1f * static_cast<f32>(paramS16(instance, 10));
            target.animated = animator.trackOf(trigger.target).has_value();
            if (target.animated) {
                animator.hold(trigger.target);
            }
            m_targets.push_back(target);
        } else if (Target* target = targetOf(trigger.target); target != nullptr) {
            // Several switches may register one lift; zero endpoints are filled by
            // subsequent registrations, rather than discarding their parameters.
            if (target->closedHeight == 0) {
                target->closedHeight = 0.1f * static_cast<f32>(paramS16(instance, 8));
                target->height = target->closedHeight;
            }
            if (target->openHeight == 0) {
                target->openHeight = 0.1f * static_cast<f32>(paramS16(instance, 10));
            }
        }
    }
    (void)collision;
    // Chains: a trigger's next is the one whose id it names, never one wanting crystals.
    for (LevelTrigger& trigger : m_triggers) {
        if (trigger.nextId == 0) {
            continue;
        }
        for (usize j = 0; j < m_triggers.size(); ++j) {
            const LevelTrigger& other = m_triggers[j];
            if (&other != &trigger && other.id == trigger.nextId &&
                (other.flags & LevelTrigger::kRequirement) == 0) {
                trigger.next = static_cast<s32>(j);
                m_triggers[j].chained = true;
                break;
            }
        }
    }
}

void LevelTriggers::clear() {
    m_figures.clear();
    m_triggers.clear();
    m_targets.clear();
    m_parents.clear();
    m_refusals.clear();
    m_openings.clear();
    m_settled.clear();
    m_cameraCues.clear();
    m_frameRemainder = 0.0f;
}

void LevelTriggers::bindFigures(RenderDevice& device, const WorldLayout& layout,
                                ItemArchive& items) {
    m_figures.clear();
    for (const auto& trigger : m_triggers) {
        const auto& instance = layout.itemInstances()[static_cast<usize>(trigger.instance)];
        const auto& info = layout.itemInfos()[static_cast<usize>(instance.info)];
        auto figure = std::make_unique<ItemFigure>();
        // Marker-only triggers have no tree. Preserve authored height: these pads
        // may sit on a moving bridge rather than on the static collision floor.
        if (!figure->place(device, items, info.name, instance, nullptr)) {
            figure.reset();
        }
        m_figures.push_back(std::move(figure));
    }
}

void LevelTriggers::draw(RenderDevice& device, const Mat4& clip,
                         const WorldLighting& lighting) const {
    for (const auto& figure : m_figures) {
        if (figure != nullptr) {
            figure->draw(device, clip, lighting);
        }
    }
}

std::vector<TriggerRefusal> LevelTriggers::takeRefusals() {
    return std::exchange(m_refusals, {});
}

std::vector<TriggerOpening> LevelTriggers::takeOpenings() {
    return std::exchange(m_openings, {});
}

std::vector<TriggerOpening> LevelTriggers::takeSettled() {
    return std::exchange(m_settled, {});
}

std::vector<TriggerCameraCue> LevelTriggers::takeCameraCues() {
    return std::exchange(m_cameraCues, {});
}

bool LevelTriggers::settled(s32 object) const {
    const Target* target = targetOf(object);
    return target == nullptr || target->settled;
}

TriggerOpening LevelTriggers::openingOf(const Target& target, bool atOnce) {
    return TriggerOpening{target.object, target.spot, (target.kind & LevelTrigger::kFades) != 0,
                          atOnce, target.sound};
}

LevelTriggers::Target* LevelTriggers::targetOf(s32 object) {
    for (Target& target : m_targets) {
        if (target.object == object) {
            return &target;
        }
    }
    return nullptr;
}

const LevelTriggers::Target* LevelTriggers::targetOf(s32 object) const {
    for (const Target& target : m_targets) {
        if (target.object == object) {
            return &target;
        }
    }
    return nullptr;
}

bool LevelTriggers::opened(s32 object) const {
    const Target* target = targetOf(object);
    return target != nullptr && target->open;
}

bool LevelTriggers::fadesAway(const Target& target) {
    // Fade mode 0x20 hides on activation; ordinary fade pads reveal a bridge
    // while occupied, then hide it again when the last participant leaves.
    return (target.kind & LevelTrigger::kOscillates) != 0 ? target.open || target.pressed
                                                          : !target.pressed;
}

void LevelTriggers::applyAlpha(const Target& target, WorldScene& scene, WorldCollision* collision) {
    for (const usize node : target.subtree) {
        scene.setObjectAlpha(node, target.alpha);
        if (collision != nullptr) {
            collision->setSolid(static_cast<s32>(node), !fadesAway(target));
        }
    }
}

f32 LevelTriggers::alphaOf(s32 object) const {
    const Target* target = targetOf(object);
    return target != nullptr ? target->alpha : 1.0f;
}

/** Crystal requirements apply to the party; a complete gargoyle collection can be shared. */
bool LevelTriggers::qualifies(const LevelTrigger& trigger,
                              std::span<const TriggerVisitor> visitors) {
    if (visitors.empty()) {
        return false;
    }
    if (trigger.needsIcons()) {
        const s32 tier = std::clamp(trigger.id - 101, 0, 2);
        return std::ranges::any_of(visitors, [tier](const TriggerVisitor& visitor) {
            const auto index = static_cast<usize>(tier);
            return visitor.gargoylePieces[index] < 0 ||
                   visitor.gargoylePieces[index] >= Relics::kGargoyleNeeded[index];
        });
    }
    if (!trigger.needsCrystals()) {
        return true;
    }
    const s32 needed = crystalsNeeded(trigger.id);
    const auto realm = static_cast<usize>(trigger.id);
    if (realm >= kRealmCount) {
        return false;
    }
    return std::ranges::all_of(
        visitors, [&](const TriggerVisitor& visitor) { return visitor.crystals[realm] >= needed; });
}

bool LevelTriggers::openTarget(Target& target, bool open, bool atOnce, WorldAnimator& animator,
                               WorldScene& scene, WorldCollision* collision) {
    if (target.open == open) {
        return false;
    }
    target.open = open;
    target.returning = false;
    target.settled = atOnce; // only an opening before the party is worth reporting done
    if (target.animated) {
        if ((target.kind & LevelTrigger::kOscillates) != 0 && !atOnce) {
            animator.cycle(target.object, open);
        } else {
            animator.fire(target.object, open, atOnce);
        }
        return true;
    }
    if ((target.kind & LevelTrigger::kFades) != 0) {
        if (atOnce) {
            target.alpha = fadesAway(target) ? 0.0f : 1.0f;
            applyAlpha(target, scene, collision);
        }
    } else if (atOnce) {
        target.height = open ? target.openHeight : target.closedHeight;
        scene.setObjectTransform(
            static_cast<usize>(target.object),
            glm::translate(Mat4{1}, target.origin + Vec3{0, target.height, 0}));
    }
    return true;
}

void LevelTriggers::fire(usize index, bool active, bool atOnce, WorldAnimator& animator,
                         WorldScene& scene, WorldCollision* collision) {
    usize followed = 0;
    for (auto at = static_cast<s32>(index); at >= 0 && followed++ < m_triggers.size();
         at = m_triggers[static_cast<usize>(at)].next) {
        LevelTrigger& trigger = m_triggers[static_cast<usize>(at)];
        const bool contact =
            active || ((trigger.flags & LevelTrigger::kKeepContact) != 0 && trigger.fired);
        const bool wasFired = trigger.fired;
        if (Target* target = targetOf(trigger.target); target != nullptr) {
            target->pressed = target->pressed || contact;
            bool open = target->open;
            if ((trigger.flags & LevelTrigger::kCloses) != 0) {
                if (contact && target->settled) {
                    open = false;
                }
                trigger.fired = !open;
            } else if ((trigger.flags & LevelTrigger::kOpensOnce) != 0) {
                if (contact && target->settled) {
                    open = true;
                }
                trigger.fired = open;
            } else if ((trigger.flags & LevelTrigger::kToggles) != 0) {
                if (contact && !target->settled) {
                    trigger.toggleCooldown = kToggleDelay;
                } else if (contact && trigger.toggleCooldown <= 0) {
                    open = !open;
                    trigger.toggleCooldown = kToggleDelay;
                } else if (!contact && (trigger.flags & 0x800U) != 0) {
                    // Empty-pad delay is one second per additional participant.
                    trigger.toggleCooldown = m_emptyToggleDelay;
                }
                trigger.fired = contact;
            } else {
                if (contact) {
                    open = true;
                }
                trigger.fired = contact;
            }
            if (openTarget(*target, open, atOnce, animator, scene, collision)) {
                target->spot = trigger.spot;
                target->sound = trigger.sound;
                m_openings.push_back(openingOf(*target, atOnce));
            }
        } else if (contact || (trigger.flags & LevelTrigger::kToggles) != 0) {
            trigger.fired = contact;
        }
        if (trigger.fired && !wasFired && contact && !atOnce) {
            m_cameraCues.push_back({trigger.id, trigger.target});
        }
        if (trigger.fired != wasFired && static_cast<usize>(at) < m_figures.size() &&
            m_figures[static_cast<usize>(at)]) {
            auto& figure = *m_figures[static_cast<usize>(at)];
            const s32 onSequence = atOnce ? 2 : 1;
            figure.play(trigger.fired ? onSequence : 3, false);
        }
    }
}

bool LevelTriggers::visited(const LevelTrigger& trigger, f32 radius,
                            std::span<const TriggerVisitor> visitors) const {
    if (visitors.empty()) {
        return false;
    }
    const auto inRange = [&](const TriggerVisitor& visitor) {
        if ((trigger.flags & LevelTrigger::kOnTarget) != 0) {
            const s32 floor = visitor.floorObject;
            if (floor != trigger.target &&
                (floor < 0 || static_cast<usize>(floor) >= m_parents.size() ||
                 m_parents[static_cast<usize>(floor)] != trigger.target)) {
                return false;
            }
        }
        const Vec3 away = visitor.position - trigger.spot;
        const f32 reach = radius + visitor.radius;
        return away.x * away.x + away.z * away.z <= reach * reach && std::abs(away.y) <= kReach;
    };
    return (trigger.flags & LevelTrigger::kWholeParty) != 0
               ? std::ranges::all_of(visitors, inRange)
               : std::ranges::any_of(visitors, inRange);
}

void LevelTriggers::openMet(std::span<const TriggerVisitor> visitors, WorldAnimator& animator,
                            WorldScene& scene, WorldCollision* collision) {
    for (usize i = 0; i < m_triggers.size(); ++i) {
        const LevelTrigger& trigger = m_triggers[i];
        if ((trigger.flags & LevelTrigger::kRequirement) != 0 && qualifies(trigger, visitors)) {
            fire(i, true, true, animator, scene, collision);
        }
    }
    for (Target& target : m_targets) {
        if ((target.kind & LevelTrigger::kFades) != 0) {
            target.alpha = fadesAway(target) ? 0.0f : 1.0f;
            applyAlpha(target, scene, collision);
        }
        if (!target.animated && (target.kind & LevelTrigger::kFades) == 0) {
            scene.setObjectTransform(
                static_cast<usize>(target.object),
                glm::translate(Mat4{1}, target.origin + Vec3{0, target.height, 0}));
        }
    }
}

void LevelTriggers::update(f32 seconds, std::span<const TriggerVisitor> visitors,
                           WorldAnimator& animator, WorldScene& scene, WorldCollision* collision) {
    m_emptyToggleDelay = visitors.empty() ? 0.0f : static_cast<f32>(visitors.size() - 1);
    for (Target& target : m_targets) {
        target.pressed = false;
    }
    for (usize i = 0; i < m_triggers.size(); ++i) {
        LevelTrigger& trigger = m_triggers[i];
        if (trigger.refusalCooldown > 0.0f) {
            trigger.refusalCooldown = std::max(trigger.refusalCooldown - seconds, 0.0f);
        }
        trigger.toggleCooldown = std::max(trigger.toggleCooldown - seconds, 0.0f);
        if (trigger.chained) {
            continue;
        }
        // Anyone standing in the spot, carrying enough, sets it off; a crystal gate's spot
        // reaches twice as far for a party that qualifies.
        const bool qualified = qualifies(trigger, visitors);
        const f32 radius =
            trigger.needsCrystals() && qualified ? trigger.radius * kMetReach : trigger.radius;
        if (!visited(trigger, radius, visitors)) {
            fire(i, false, false, animator, scene, collision);
            continue;
        }
        if (qualified) {
            fire(i, true, false, animator, scene, collision);
        } else if ((trigger.flags & LevelTrigger::kRequirement) != 0 &&
                   trigger.refusalCooldown <= 0.0f) {
            // Told once what the spot wants, then not again for a while.
            m_refusals.push_back(
                TriggerRefusal{static_cast<s32>(i), trigger.id, trigger.needsCrystals()});
            trigger.refusalCooldown = kRefusalCooldown;
        }
    }
    // An unoccupied pad must not cancel another pad's contact, or reset a target
    // registered as a latch by its first switch.
    for (Target& target : m_targets) {
        if ((target.kind & 7U) == 0 && !target.pressed &&
            openTarget(target, false, false, animator, scene, collision)) {
            m_openings.push_back(openingOf(target, false));
        }
    }
    // Fields thin out a step a game frame.
    m_frameRemainder += seconds * kFrameRate;
    const f32 frames = std::floor(m_frameRemainder);
    m_frameRemainder -= frames;
    for (Target& target : m_targets) {
        if (!target.animated && (target.kind & LevelTrigger::kFades) == 0) {
            if (!target.settled) {
                // GameCube items.c ProcessItemWobjs: 4.0 * gClockFrameStep.
                const f32 goal =
                    target.open && !target.returning ? target.openHeight : target.closedHeight;
                const f32 distance = goal - target.height;
                const f32 step = kHeightSpeed * seconds;
                target.height += std::clamp(distance, -step, step);
                if (std::abs(goal - target.height) <= kHeightTolerance) {
                    target.height = goal;
                    target.settled = true;
                    if (std::abs(distance) > kHeightTolerance) {
                        m_settled.push_back(openingOf(target, false));
                    }
                    if (target.open && (target.kind & LevelTrigger::kOscillates) != 0 &&
                        target.openHeight != target.closedHeight) {
                        target.returning = !target.returning;
                        target.settled = false;
                    }
                }
                if (collision != nullptr && (target.kind & LevelTrigger::kStaysSolid) == 0) {
                    collision->setSolid(target.object, target.settled);
                }
            }
            scene.setObjectTransform(
                static_cast<usize>(target.object),
                glm::translate(Mat4{1}, target.origin + Vec3{0, target.height, 0}));
        }
        if ((target.kind & LevelTrigger::kFades) == 0) {
            continue;
        }
        const f32 goal = fadesAway(target) ? 0.0f : 1.0f;
        if (target.alpha != goal) {
            target.settled = false;
        }
        target.alpha += std::clamp(goal - target.alpha, -kFadeRate * frames, kFadeRate * frames);
        applyAlpha(target, scene, collision);
        if (!target.settled && target.alpha == goal) {
            target.settled = true;
            m_settled.push_back(openingOf(target, false));
        }
    }
    for (usize i = 0; i < m_figures.size(); ++i) {
        if (m_figures[i] == nullptr) {
            continue;
        }
        auto& figure = *m_figures[i];
        figure.update(seconds);
        if (m_triggers[i].fired && figure.sequence() == 1 && figure.finished()) {
            figure.play(2, true);
        } else if (!m_triggers[i].fired && figure.sequence() == 3 && figure.finished()) {
            figure.play(0, true);
        }
    }
    // An animated target is done once its animation has run to the end.
    for (Target& target : m_targets) {
        if (target.settled || !target.animated) {
            continue;
        }
        if (const auto track = animator.trackOf(target.object);
            track.has_value() && animator.finished(*track)) {
            target.settled = true;
            m_settled.push_back(openingOf(target, false));
        }
        if (collision != nullptr && (target.kind & LevelTrigger::kStaysSolid) == 0) {
            collision->setSolid(target.object, target.settled);
        }
    }
}

} // namespace gdl::game
