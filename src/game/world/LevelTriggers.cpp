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
constexpr f32 kFloorAbove = 4.0f;
constexpr f32 kFloorBelow = 10.0f;
constexpr f32 kFloorRadius = 1.0f;
constexpr f32 kFloorLift = 0.1f;
constexpr s32 kPlayerSlots = 4;

u32 visitorBit(const TriggerVisitor& visitor, usize index) {
    const auto slot = visitor.party >= 0 ? static_cast<usize>(visitor.party) : index;
    return slot < static_cast<usize>(kPlayerSlots) ? 1U << slot : 0;
}

s16 paramS16(const ItemInstance& instance, usize at) {
    s16 value = 0;
    std::memcpy(&value, &instance.params[at], sizeof(value));
    return value;
}

u32 flagsOf(const ItemInfo& info, const ItemInstance& instance) {
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
    return flags | (params & ~0xFFU);
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
    // Markers can be authored against an animation's initial pose (J4's lowered
    // platforms), or its rest pose (G1's lift pad). Prefer the displayed floor;
    // use rest geometry for unsupported markers or their explicitly named lift.
    std::optional<WorldCollision> restCollision;
    if (collision != nullptr && collision->movingObjectCount() != 0) {
        restCollision = *collision;
        for (usize i = 0; i < layout.objects().size(); ++i) {
            restCollision->setObjectTransform(static_cast<s32>(i),
                                              glm::translate(Mat4{1.0f}, layout.worldPosition(i)));
        }
    }
    for (const auto& object : layout.objects()) {
        m_parents.push_back(object.parent);
    }
    const std::vector<ItemInfo>& infos = layout.itemInfos();
    const std::vector<ItemInstance>& instances = layout.itemInstances();
    // Height-driven floors must be at their closed endpoint before contact markers
    // choose a supporting floor, just like keyed floors already are. Otherwise an
    // upper landing's call switch can attach to a lift that starts below it.
    std::optional<WorldCollision> initialCollision;
    if (collision != nullptr) {
        initialCollision = *collision;
        std::vector<f32> heights(layout.objects().size(), 0.0f);
        for (const auto& instance : instances) {
            if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size()) {
                continue;
            }
            const auto& info = infos[static_cast<usize>(instance.info)];
            const s32 object = paramS16(instance, 0);
            if (info.type != ItemInfo::kTrigger || object < 0 ||
                static_cast<usize>(object) >= heights.size() ||
                (flagsOf(info, instance) & LevelTrigger::kFades) != 0 ||
                animator.trackOf(object).has_value()) {
                continue;
            }
            auto& height = heights[static_cast<usize>(object)];
            if (height == 0.0f) {
                height = 0.1f * static_cast<f32>(paramS16(instance, 8));
            }
        }
        for (usize i = 0; i < heights.size(); ++i) {
            if (auto placement = collision->objectTransform(static_cast<s32>(i))) {
                auto node = static_cast<s32>(i);
                for (usize guard = 0; node >= 0 && guard < heights.size(); ++guard) {
                    const auto at = static_cast<usize>(node);
                    if (heights[at] != 0.0f) {
                        // Replace, rather than add to, an existing endpoint: callers
                        // may bind against collision that is already initialized.
                        if (const auto current = collision->objectTransform(node)) {
                            f32 initialY = layout.worldPosition(at).y;
                            auto parent = node;
                            for (usize depth = 0; parent >= 0 && depth < heights.size(); ++depth) {
                                initialY += heights[static_cast<usize>(parent)];
                                parent = m_parents[static_cast<usize>(parent)];
                            }
                            (*placement)[3].y += initialY - (*current)[3].y;
                        }
                        break;
                    }
                    node = m_parents[at];
                }
                initialCollision->setObjectTransform(static_cast<s32>(i), *placement);
            }
        }
    }
    const auto* initialFloors = initialCollision ? &*initialCollision : collision;
    for (usize i = 0; i < instances.size(); ++i) {
        const ItemInstance& instance = instances[i];
        if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size() ||
            infos[static_cast<usize>(instance.info)].type != ItemInfo::kTrigger) {
            continue;
        }
        const ItemInfo& info = infos[static_cast<usize>(instance.info)];
        LevelTrigger trigger;
        trigger.instance = static_cast<s32>(i);
        trigger.minPlayers = instance.minPlayers;
        trigger.spot = instance.position;
        trigger.placement = itemPlacement(instance.position, instance.rotation);
        const s16 object = paramS16(instance, 0);
        trigger.target =
            object >= 0 && static_cast<usize>(object) < layout.objects().size() ? object : -1;
        const u32 params = static_cast<u16>(paramS16(instance, 2));
        const auto onTarget = [&](const FloorHit& floor) {
            return floor.object == trigger.target ||
                   (floor.object >= 0 && static_cast<usize>(floor.object) < m_parents.size() &&
                    m_parents[static_cast<usize>(floor.object)] == trigger.target);
        };
        std::optional<FloorHit> support;
        const WorldCollision* supportCollision = initialFloors;
        if (collision != nullptr && (info.collisionFlags & 1U) == 0) {
            support = initialFloors->floorAt(trigger.spot, kFloorAbove, kFloorBelow, kFloorRadius);
            if (restCollision &&
                (!support || ((params & LevelTrigger::kOnTarget) != 0 && !onTarget(*support)))) {
                const auto rest =
                    restCollision->floorAt(trigger.spot, kFloorAbove, kFloorBelow, kFloorRadius);
                // An explicitly floor-bound pad must not attach to a nearby still
                // deck just because its own lift is currently at the other endpoint.
                if (rest && (!support || onTarget(*rest))) {
                    supportCollision = &*restCollision;
                    support = rest;
                }
            }
            if (support) {
                // AddItemSub first rests the item on its floor, then parents it.
                // Several lift markers are authored well above the deck itself.
                trigger.spot.y = support->y + kFloorLift;
                trigger.placement[3] = Vec4{trigger.spot, 1};
            }
            if (support && (support->objectFlags & WorldObject::kAnimated) != 0) {
                if (const auto placement = supportCollision->objectTransform(support->object)) {
                    trigger.floor = support->object;
                    trigger.localPlacement = glm::inverse(*placement) * trigger.placement;
                    if (const auto current = initialFloors->objectTransform(trigger.floor)) {
                        trigger.placement = *current * trigger.localPlacement;
                        trigger.spot = Vec3{trigger.placement[3]};
                    }
                }
            }
        }
        // The trigger's flags: the kind's own, then whatever the instance adds.
        u32 flags = flagsOf(info, instance);
        if (support && trigger.target >= 0 && (flags & LevelTrigger::kWholeParty) != 0 &&
            (support->object == trigger.target ||
             (support->object >= 0 && static_cast<usize>(support->object) < m_parents.size() &&
              m_parents[static_cast<usize>(support->object)] == trigger.target))) {
            flags |= LevelTrigger::kOnTarget;
        }
        trigger.flags = flags;
        trigger.kind = flags & 0xFFU;
        trigger.radius =
            instance.params[4] == kTinyRadius ? 0.01f : 0.5f * static_cast<f32>(instance.params[4]);
        if (trigger.radius <= 0.0f) {
            trigger.radius = info.radius; // the kind's own, when the instance gives none
        }
        trigger.id = instance.params[6];
        trigger.nextId = instance.params[7];
        trigger.shootable = info.subtype == LevelTrigger::kShootableSubtype;
        trigger.height = info.height;
        trigger.movementLesson = info.subtype == 23 && trigger.target >= 0;
        // The slot is a signed byte in the data: 255 (and anything high) means none.
        const u8 slot = instance.params[5];
        trigger.sound = slot >= 0x80 ? -1 : static_cast<s32>(slot);
        m_triggers.push_back(trigger);
        if (trigger.target >= 0 && targetOf(trigger.target) == nullptr) {
            Target target;
            target.subtype = info.subtype;
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
    refreshEligibility(1);
}

void LevelTriggers::clear() {
    m_figures.clear();
    m_triggers.clear();
    m_targets.clear();
    m_parents.clear();
    m_refusals.clear();
    m_lessons.clear();
    m_openings.clear();
    m_settled.clear();
    m_cameraCues.clear();
    m_wakes.clear();
    m_frameRemainder = 0.0f;
    m_emptyToggleDelay = 0.0f;
    m_playerCount = -1;
    m_cameraHeld = false;
}

void LevelTriggers::setPlayerCount(s32 players) {
    m_playerCount = std::clamp(players, 0, kPlayerSlots);
    refreshEligibility(m_playerCount);
}

void LevelTriggers::refreshEligibility(s32 players) {
    for (LevelTrigger& trigger : m_triggers) {
        trigger.enabled = shownToParty(trigger.minPlayers, players);
        if (!trigger.enabled) {
            trigger.occupied = false;
            trigger.heldContacts = 0;
            trigger.shot = false;
        }
    }
}

void LevelTriggers::bindFigures(RenderDevice& device, const WorldLayout& layout,
                                ItemArchive& items) {
    m_figures.clear();
    for (const auto& trigger : m_triggers) {
        const auto& instance = layout.itemInstances()[static_cast<usize>(trigger.instance)];
        const auto& info = layout.itemInfos()[static_cast<usize>(instance.info)];
        constexpr u32 kNoGeometry = 2;
        if ((instance.flags & kNoGeometry) != 0) {
            m_figures.push_back(nullptr);
            continue;
        }
        auto figure = std::make_unique<ItemFigure>();
        // Marker-only triggers have no tree. Preserve authored height: these pads
        // may sit on a moving bridge rather than on the static collision floor.
        if (!figure->place(device, items, info.name, instance, nullptr)) {
            figure.reset();
        } else {
            figure->placeAt(trigger.placement);
        }
        m_figures.push_back(std::move(figure));
    }
}

void LevelTriggers::draw(RenderDevice& device, const Mat4& clip,
                         const WorldLighting& lighting) const {
    for (usize i = 0; i < m_figures.size(); ++i) {
        if (m_figures[i] != nullptr && m_triggers[i].enabled) {
            m_figures[i]->draw(device, clip, lighting);
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
    return TriggerOpening{target.object, target.spot,  (target.kind & LevelTrigger::kFades) != 0,
                          atOnce,        target.sound, target.subtype,
                          !target.open};
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

/** One member meeting a crystal or gargoyle requirement opens it for the whole party. */
bool LevelTriggers::qualifies(const LevelTrigger& trigger,
                              std::span<const TriggerVisitor> visitors) {
    if (visitors.empty()) {
        return false;
    }
    if (trigger.needsIcons()) {
        const s32 tier = std::clamp(trigger.id - 101, 0, 2);
        return std::ranges::any_of(visitors, [tier](const TriggerVisitor& visitor) {
            const auto index = static_cast<usize>(tier);
            return visitor.sumner || visitor.gargoylePieces[index] < 0 ||
                   visitor.gargoylePieces[index] >= Relics::kGargoyleNeeded[index];
        });
    }
    if (!trigger.needsCrystals()) {
        return true;
    }
    return std::ranges::any_of(
        visitors, [&](const TriggerVisitor& visitor) { return crystalsMet(visitor, trigger.id); });
}

bool LevelTriggers::crystalsMet(const TriggerVisitor& visitor, s32 realm) {
    if (realm < 0 || static_cast<usize>(realm) >= kRealmCount) {
        return false;
    }
    const s32 crystals = visitor.crystals[static_cast<usize>(realm)];
    return visitor.sumner || crystals < 0 || crystals >= crystalsNeeded(realm);
}

bool LevelTriggers::openTarget(Target& target, bool open, bool atOnce, WorldAnimator& animator,
                               WorldScene& scene, WorldCollision* collision) {
    if ((!open && target.forced) || (target.open == open && (!atOnce || target.settled))) {
        return false;
    }
    target.open = open;
    target.returning = false;
    target.settled = atOnce; // only an opening before the party is worth reporting done
    if (atOnce && collision != nullptr) {
        collision->setFloorExitBlocked(target.object, false);
    }
    if (target.animated) {
        if (!target.forced && (target.kind & LevelTrigger::kOscillates) != 0 && !atOnce) {
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
        fireNode(static_cast<usize>(at), active, atOnce, animator, scene, collision);
    }
}

void LevelTriggers::fireNode(usize index, bool active, bool atOnce, WorldAnimator& animator,
                             WorldScene& scene, WorldCollision* collision) {
    LevelTrigger& trigger = m_triggers[index];
    if (trigger.forced || !trigger.enabled) {
        return;
    }
    const bool contact = active;
    const bool wasFired = trigger.fired;
    if (Target* target = targetOf(trigger.target); target != nullptr) {
        target->pressed = target->pressed || contact;
        if ((trigger.flags & LevelTrigger::kWholeParty) != 0) {
            target->wholePartyReady = target->wholePartyReady || active;
        }
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
        m_cameraCues.push_back({trigger.id, trigger.target, (trigger.flags & 0x1000U) != 0});
        if ((trigger.flags & LevelTrigger::kWakesStatue) != 0) {
            m_wakes.push_back(trigger.spot);
        }
    }
    if (trigger.fired != wasFired && index < m_figures.size() && m_figures[index]) {
        auto& figure = *m_figures[index];
        const s32 onSequence = atOnce ? 2 : 1;
        figure.play(trigger.fired ? onSequence : 3, false);
    }
}

bool LevelTriggers::onTarget(const LevelTrigger& trigger, const TriggerVisitor& visitor) const {
    if ((trigger.flags & LevelTrigger::kOnTarget) != 0) {
        const s32 floor = visitor.floorObject;
        if (floor != trigger.target &&
            (floor < 0 || static_cast<usize>(floor) >= m_parents.size() ||
             m_parents[static_cast<usize>(floor)] != trigger.target)) {
            return false;
        }
    }
    return true;
}

bool LevelTriggers::reaches(const LevelTrigger& trigger, f32 radius,
                            const TriggerVisitor& visitor) const {
    if (!onTarget(trigger, visitor)) {
        return false;
    }
    const Vec3 away = visitor.position - trigger.spot;
    const f32 reach = radius + visitor.radius;
    return away.x * away.x + away.z * away.z <= reach * reach &&
           std::abs(away.y) <= trigger.height + visitor.height * 0.5f;
}

void LevelTriggers::collectContacts(usize index, u32 mask, std::span<u32> contacts) const {
    // Contact identities travel through the entire chain. Each node's party-size,
    // floor and whole-party requirements govern that node, not later successors.
    usize followed = 0;
    for (auto at = static_cast<s32>(index); at >= 0 && followed++ < m_triggers.size();
         at = m_triggers[static_cast<usize>(at)].next) {
        contacts[static_cast<usize>(at)] |= mask;
    }
}

void LevelTriggers::shoot(usize index) {
    if (index < m_triggers.size() && m_triggers[index].shootable) {
        m_triggers[index].shot = true;
    }
}

void LevelTriggers::openMet(std::span<const TriggerVisitor> visitors, WorldAnimator& animator,
                            WorldScene& scene, WorldCollision* collision) {
    refreshEligibility(m_playerCount >= 0 ? m_playerCount : static_cast<s32>(visitors.size()));
    for (usize i = 0; i < m_triggers.size(); ++i) {
        const LevelTrigger& trigger = m_triggers[i];
        if (trigger.enabled && (trigger.flags & LevelTrigger::kRequirement) != 0 &&
            qualifies(trigger, visitors)) {
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

void LevelTriggers::openAtOnce(std::span<const s32> ids, WorldAnimator& animator, WorldScene& scene,
                               WorldCollision* collision) {
    for (const LevelTrigger& trigger : m_triggers) {
        if (std::ranges::find(ids, trigger.id) == ids.end()) {
            continue;
        }
        Target* target = targetOf(trigger.target);
        if (target != nullptr && openTarget(*target, true, true, animator, scene, collision)) {
            target->spot = trigger.spot;
            target->sound = trigger.sound;
            m_openings.push_back(openingOf(*target, true));
        }
    }
}

void LevelTriggers::activate(s32 id, bool atOnce, WorldAnimator& animator, WorldScene& scene,
                             WorldCollision* collision) {
    for (usize i = 0; i < m_triggers.size(); ++i) {
        if (m_triggers[i].id != id) {
            continue;
        }
        s32 at = static_cast<s32>(i);
        for (usize guard = 0; at >= 0 && guard < m_triggers.size(); ++guard) {
            auto& trigger = m_triggers[static_cast<usize>(at)];
            trigger.forced = true;
            trigger.fired = true;
            if (Target* target = targetOf(trigger.target)) {
                target->forced = true;
                if (openTarget(*target, true, atOnce, animator, scene, collision)) {
                    target->spot = trigger.spot;
                    target->sound = trigger.sound;
                    m_openings.push_back(openingOf(*target, atOnce));
                }
            }
            at = trigger.next;
        }
    }
}

void LevelTriggers::update(f32 seconds, std::span<const TriggerVisitor> visitors,
                           WorldAnimator& animator, WorldScene& scene, WorldCollision* collision) {
    refreshEligibility(m_playerCount >= 0 ? m_playerCount : static_cast<s32>(visitors.size()));
    m_emptyToggleDelay = visitors.empty() ? 0.0f : static_cast<f32>(visitors.size() - 1);
    u32 standingMask = 0;
    for (usize i = 0; i < visitors.size(); ++i) {
        standingMask |= visitorBit(visitors[i], i);
    }
    std::vector<u32> contacts(m_triggers.size(), 0);
    for (Target& target : m_targets) {
        target.pressed = false;
        target.wholePartyReady = false;
    }
    for (usize i = 0; i < m_triggers.size(); ++i) {
        LevelTrigger& trigger = m_triggers[i];
        if (collision != nullptr && trigger.floor >= 0) {
            if (const auto floor = collision->objectTransform(trigger.floor)) {
                const Mat4 placement = *floor * trigger.localPlacement;
                trigger.placement = placement;
                trigger.spot = Vec3{placement[3]};
                if (i < m_figures.size() && m_figures[i] != nullptr) {
                    m_figures[i]->placeAt(placement);
                }
            }
        }
    }
    for (usize i = 0; i < m_triggers.size(); ++i) {
        LevelTrigger& trigger = m_triggers[i];
        if (trigger.forced || !trigger.enabled) {
            continue;
        }
        if (trigger.refusalCooldown > 0.0f) {
            trigger.refusalCooldown = std::max(trigger.refusalCooldown - seconds, 0.0f);
        }
        trigger.toggleCooldown = std::max(trigger.toggleCooldown - seconds, 0.0f);
        // Shot, it goes off whether it heads a chain or follows one.
        if (trigger.shot) {
            trigger.shot = false;
            if ((trigger.flags & LevelTrigger::kRequirement) == 0 || qualifies(trigger, visitors)) {
                collectContacts(i, standingMask, contacts);
                continue;
            }
        }
        if (trigger.chained) {
            continue;
        }
        // Anyone standing in the spot, carrying enough, sets it off; a crystal gate's spot
        // reaches twice as far for a party that qualifies.
        const bool qualified = qualifies(trigger, visitors);
        const f32 radius =
            trigger.needsCrystals() && qualified ? trigger.radius * kMetReach : trigger.radius;
        u32 mask = 0;
        const TriggerVisitor* first = nullptr;
        for (usize visitor = 0; visitor < visitors.size(); ++visitor) {
            if (reaches(trigger, radius, visitors[visitor])) {
                mask |= visitorBit(visitors[visitor], visitor);
                if (first == nullptr) {
                    first = &visitors[visitor];
                }
            }
        }
        // A direct contact contributes its player's bit even when that root itself
        // needs the whole party. Other root spots may contribute the missing bits.
        if ((trigger.flags & LevelTrigger::kWholeParty) != 0) {
            if (first != nullptr && !trigger.occupied && visitors.size() > 1) {
                m_lessons.push_back(
                    TriggerLesson{first->party, (trigger.flags & LevelTrigger::kOnTarget) != 0});
            }
            trigger.occupied = first != nullptr;
        }
        if (mask == 0) {
            continue;
        }
        if (qualified) {
            collectContacts(i, mask, contacts);
        } else if ((trigger.flags & LevelTrigger::kRequirement) != 0 &&
                   trigger.refusalCooldown <= 0.0f) {
            // Told once what the spot wants, then not again for a while.
            m_refusals.push_back(
                TriggerRefusal{static_cast<s32>(i), trigger.id, trigger.needsCrystals()});
            trigger.refusalCooldown = kRefusalCooldown;
        }
    }
    // Evaluate each node once, after every contact root has supplied its visitors.
    // An unavailable linked node is skipped locally, not a break in the chain.
    for (usize i = 0; i < m_triggers.size(); ++i) {
        auto& trigger = m_triggers[i];
        if (trigger.forced || !trigger.enabled) {
            continue;
        }
        u32 mask = contacts[i];
        if (m_cameraHeld || (trigger.flags & LevelTrigger::kKeepContact) != 0) {
            mask |= trigger.heldContacts;
        }
        mask &= standingMask;
        for (usize visitor = 0; visitor < visitors.size(); ++visitor) {
            if (!onTarget(trigger, visitors[visitor])) {
                mask &= ~visitorBit(visitors[visitor], visitor);
            }
        }
        if ((trigger.flags & LevelTrigger::kWholeParty) != 0 && mask != standingMask) {
            mask = 0;
        }
        if (!qualifies(trigger, visitors)) {
            mask = 0;
        }
        trigger.heldContacts = mask;
        fireNode(i, mask != 0, false, animator, scene, collision);
    }
    // An unoccupied pad must not cancel another pad's contact, or reset a target
    // registered as a latch by its first switch.
    for (Target& target : m_targets) {
        if (!target.forced && (target.kind & 7U) == 0 && !target.pressed &&
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
                    if (!target.forced && target.open &&
                        (target.kind & LevelTrigger::kOscillates) != 0 &&
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
        if (m_figures[i] == nullptr || !m_triggers[i].enabled) {
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
    if (collision != nullptr) {
        for (const Target& target : m_targets) {
            // ProcessItemWobjs / PlayerNewFloor: active height targets, or animated
            // lifts carrying the whole party, prohibit changing floors until stopped.
            const bool heightTarget = !target.animated && (target.kind & LevelTrigger::kFades) == 0;
            collision->setFloorExitBlocked(
                target.object, !target.settled && (heightTarget || target.wholePartyReady));
        }
    }
}

} // namespace gdl::game
