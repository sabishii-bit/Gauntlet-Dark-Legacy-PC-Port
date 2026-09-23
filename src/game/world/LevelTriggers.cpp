#include "game/world/LevelTriggers.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace gdl::game {

namespace {

constexpr std::uint32_t kTriggerKind = 24; ///< the item subtype the tower's triggers use
constexpr std::uint32_t kBridgeKind = 20;
constexpr std::uint32_t kBridgeFlags = 0x10;
constexpr std::uint32_t kDefaultFlags = 0x8;
constexpr std::uint8_t kTinyRadius = 0xFF;

std::int16_t paramS16(const ItemInstance& instance, std::size_t at) {
    std::int16_t value = 0;
    std::memcpy(&value, &instance.params[at], sizeof(value));
    return value;
}

} // namespace

std::int32_t LevelTriggers::crystalsNeeded(std::int32_t realm) {
    if (realm < 0 || static_cast<std::size_t>(realm) >= kCrystalsToOpen.size()) {
        return 0;
    }
    return kCrystalsToOpen[static_cast<std::size_t>(realm)];
}

void LevelTriggers::bind(const WorldLayout& layout, WorldAnimator& animator,
                         WorldCollision* collision) {
    clear();
    const std::vector<ItemInfo>& infos = layout.itemInfos();
    const std::vector<ItemInstance>& instances = layout.itemInstances();
    for (std::size_t i = 0; i < instances.size(); ++i) {
        const ItemInstance& instance = instances[i];
        if (instance.info < 0 || static_cast<std::size_t>(instance.info) >= infos.size() ||
            infos[static_cast<std::size_t>(instance.info)].type != ItemInfo::kTrigger) {
            continue;
        }
        const ItemInfo& info = infos[static_cast<std::size_t>(instance.info)];
        LevelTrigger trigger;
        trigger.instance = static_cast<std::int32_t>(i);
        trigger.spot = instance.position;
        const std::int16_t object = paramS16(instance, 0);
        trigger.target =
            object >= 0 && static_cast<std::size_t>(object) < layout.objects().size() ? object : -1;
        // The trigger's flags: the kind's own, then whatever the instance adds.
        std::uint32_t flags =
            static_cast<std::uint32_t>(info.subtype) == kBridgeKind ? kBridgeFlags : kDefaultFlags;
        if (static_cast<std::uint32_t>(info.subtype) == kTriggerKind ||
            static_cast<std::uint32_t>(info.subtype) > kTriggerKind) {
            flags = static_cast<std::uint32_t>(static_cast<std::uint16_t>(paramS16(instance, 2))) |
                    kDefaultFlags;
        }
        trigger.flags = flags;
        trigger.kind = flags & 0xFFU;
        trigger.radius = instance.params[4] == kTinyRadius
                             ? 0.01f
                             : 0.5f * static_cast<float>(instance.params[4]);
        if (trigger.radius <= 0.0f) {
            trigger.radius = info.radius; // the kind's own, when the instance gives none
        }
        trigger.id = instance.params[6];
        trigger.nextId = instance.params[7];
        // The slot is a signed byte in the data: 255 (and anything high) means none.
        const std::uint8_t slot = instance.params[5];
        trigger.sound = slot >= 0x80 ? -1 : static_cast<std::int32_t>(slot);
        m_triggers.push_back(trigger);
        if (trigger.target >= 0 && targetOf(trigger.target) == nullptr) {
            Target target;
            target.object = trigger.target;
            target.kind = trigger.kind;
            target.animated = animator.trackOf(trigger.target).has_value();
            if (target.animated) {
                animator.hold(trigger.target);
            }
            m_targets.push_back(target);
        }
    }
    (void)collision;
    // Chains: a trigger's next is the one whose id it names, never one wanting crystals.
    for (LevelTrigger& trigger : m_triggers) {
        if (trigger.nextId == 0) {
            continue;
        }
        for (std::size_t j = 0; j < m_triggers.size(); ++j) {
            const LevelTrigger& other = m_triggers[j];
            if (&other != &trigger && other.id == trigger.nextId &&
                (other.flags & LevelTrigger::kRequirement) == 0) {
                trigger.next = static_cast<std::int32_t>(j);
                m_triggers[j].chained = true;
                break;
            }
        }
    }
}

void LevelTriggers::clear() {
    m_triggers.clear();
    m_targets.clear();
    m_refusals.clear();
    m_openings.clear();
    m_settled.clear();
    m_frameRemainder = 0.0f;
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

TriggerOpening LevelTriggers::openingOf(const Target& target, bool atOnce) {
    return TriggerOpening{target.object, target.spot, (target.kind & LevelTrigger::kFades) != 0,
                          atOnce, target.sound};
}

LevelTriggers::Target* LevelTriggers::targetOf(std::int32_t object) {
    for (Target& target : m_targets) {
        if (target.object == object) {
            return &target;
        }
    }
    return nullptr;
}

const LevelTriggers::Target* LevelTriggers::targetOf(std::int32_t object) const {
    for (const Target& target : m_targets) {
        if (target.object == object) {
            return &target;
        }
    }
    return nullptr;
}

bool LevelTriggers::opened(std::int32_t object) const {
    const Target* target = targetOf(object);
    return target != nullptr && target->open;
}

float LevelTriggers::alphaOf(std::int32_t object) const {
    const Target* target = targetOf(object);
    return target != nullptr ? target->alpha : 1.0f;
}

/** Whether every visitor carries what the trigger asks for. */
bool LevelTriggers::qualifies(const LevelTrigger& trigger,
                              std::span<const TriggerVisitor> visitors) {
    if (trigger.needsIcons()) {
        return false; // the golden icons are not gathered yet
    }
    if (!trigger.needsCrystals()) {
        return true;
    }
    const std::int32_t needed = crystalsNeeded(trigger.id);
    const auto realm = static_cast<std::size_t>(trigger.id);
    if (realm >= kRealmCount) {
        return false;
    }
    return std::ranges::all_of(
        visitors, [&](const TriggerVisitor& visitor) { return visitor.crystals[realm] >= needed; });
}

bool LevelTriggers::openTarget(Target& target, bool atOnce, WorldAnimator& animator,
                               WorldScene& scene, WorldCollision* collision) {
    if (target.open) {
        return false;
    }
    target.open = true;
    target.settled = atOnce; // only an opening before the party is worth reporting done
    if (target.animated) {
        animator.fire(target.object, true, atOnce);
        return true;
    }
    if ((target.kind & LevelTrigger::kFades) != 0) {
        // A field stops blocking as soon as it starts to thin.
        if (collision != nullptr) {
            collision->setSolid(target.object, false);
        }
        if (atOnce) {
            target.alpha = 0.0f;
            scene.setObjectAlpha(static_cast<std::size_t>(target.object), 0.0f);
        }
    }
    return true;
}

void LevelTriggers::fire(std::size_t index, bool atOnce, WorldAnimator& animator, WorldScene& scene,
                         WorldCollision* collision) {
    for (std::int32_t at = static_cast<std::int32_t>(index); at >= 0;
         at = m_triggers[static_cast<std::size_t>(at)].next) {
        LevelTrigger& trigger = m_triggers[static_cast<std::size_t>(at)];
        if (trigger.fired) {
            break;
        }
        trigger.fired = true;
        if (Target* target = targetOf(trigger.target); target != nullptr && !target->open) {
            target->spot = trigger.spot;
            target->sound = trigger.sound;
            if (openTarget(*target, atOnce, animator, scene, collision)) {
                m_openings.push_back(openingOf(*target, atOnce));
            }
        }
    }
}

bool LevelTriggers::visited(const LevelTrigger& trigger, float radius,
                            std::span<const TriggerVisitor> visitors) {
    return std::ranges::any_of(visitors, [&](const TriggerVisitor& visitor) {
        const Vec3 away = visitor.position - trigger.spot;
        const float reach = radius + visitor.radius;
        return away.x * away.x + away.z * away.z <= reach * reach && std::abs(away.y) <= kReach;
    });
}

void LevelTriggers::openMet(std::span<const TriggerVisitor> visitors, WorldAnimator& animator,
                            WorldScene& scene, WorldCollision* collision) {
    for (std::size_t i = 0; i < m_triggers.size(); ++i) {
        LevelTrigger& trigger = m_triggers[i];
        if (trigger.needsCrystals() && qualifies(trigger, visitors)) {
            fire(i, true, animator, scene, collision);
        } else if (!trigger.fired) {
            trigger.occupied = visited(trigger, trigger.radius, visitors);
        }
    }
}

void LevelTriggers::update(float seconds, std::span<const TriggerVisitor> visitors,
                           WorldAnimator& animator, WorldScene& scene, WorldCollision* collision) {
    for (std::size_t i = 0; i < m_triggers.size(); ++i) {
        LevelTrigger& trigger = m_triggers[i];
        if (trigger.refusalCooldown > 0.0f) {
            trigger.refusalCooldown = std::max(trigger.refusalCooldown - seconds, 0.0f);
        }
        if (trigger.fired || (trigger.flags & LevelTrigger::kCloses) != 0 || trigger.chained) {
            continue;
        }
        // Anyone standing in the spot, carrying enough, sets it off; a crystal gate's spot
        // reaches twice as far for a party that qualifies.
        const bool qualified = qualifies(trigger, visitors);
        const float radius =
            trigger.needsCrystals() && qualified ? trigger.radius * kMetReach : trigger.radius;
        if (!visited(trigger, radius, visitors)) {
            trigger.occupied = false;
            continue;
        }
        if (qualified) {
            if (trigger.occupied) {
                continue; // stood in from the start: not until they come back to it
            }
            fire(i, false, animator, scene, collision);
        } else if ((trigger.flags & LevelTrigger::kRequirement) != 0 &&
                   trigger.refusalCooldown <= 0.0f) {
            // Told once what the spot wants, then not again for a while.
            m_refusals.push_back(
                TriggerRefusal{static_cast<std::int32_t>(i), trigger.id, trigger.needsCrystals()});
            trigger.refusalCooldown = kRefusalCooldown;
        }
    }
    // Fields thin out a step a game frame.
    m_frameRemainder += seconds * kFrameRate;
    const float frames = std::floor(m_frameRemainder);
    m_frameRemainder -= frames;
    for (Target& target : m_targets) {
        if (!target.open || (target.kind & LevelTrigger::kFades) == 0 || target.alpha <= 0.0f) {
            continue;
        }
        target.alpha = std::max(target.alpha - kFadeRate * frames, 0.0f);
        scene.setObjectAlpha(static_cast<std::size_t>(target.object), target.alpha);
        if (target.alpha <= 0.0f && !target.settled) {
            target.settled = true;
            m_settled.push_back(openingOf(target, false));
        }
    }
    // An animated target is done once its animation has run to the end.
    for (Target& target : m_targets) {
        if (!target.open || target.settled || !target.animated) {
            continue;
        }
        if (const auto track = animator.trackOf(target.object);
            track.has_value() && animator.finished(*track)) {
            target.settled = true;
            m_settled.push_back(openingOf(target, false));
        }
    }
}

} // namespace gdl::game
