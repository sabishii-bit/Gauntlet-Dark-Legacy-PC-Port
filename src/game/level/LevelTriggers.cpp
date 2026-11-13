#include "game/level/LevelTriggers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_set>
#include <utility>

#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"

#include "game/level/NativeStateResources.h"

namespace gdl::game {

bool LevelTriggers::registerNativeResources(NativeStateResources& resources) const {
    for (const auto& figure : m_sim.figures) {
        if (figure && !figure->registerNativeResources(resources)) {
            return false;
        }
    }
    return resources.good();
}

void visitNativeState(NativeStateDigest& digest, const TriggerRefusal& value) {
    digest.fields<3>(value);
}
void visitNativeState(NativeStateDigest& digest, const TriggerOpening& value) {
    digest.fields<7>(value);
}
void visitNativeState(NativeStateDigest& digest, const TriggerCameraCue& value) {
    digest.fields<3>(value);
}
void visitNativeState(NativeStateDigest& digest, const TriggerLesson& value) {
    digest.fields<2>(value);
}
void visitNativeState(NativeStateDigest& digest, const LevelTrigger& value) {
    digest.fields<26>(value);
}
void visitNativeState(NativeStateDigest& digest, const LevelTriggers::Target& value) {
    digest.fields<19>(value);
}

void LevelTriggers::appendSimulationDigest(NativeStateDigest& digest, const Simulation& state) {
    digest.fields<13>(state);
}

bool LevelTriggers::appendNativeDigest(NativeStateDigest& digest) const {
    digest.string("LevelTriggers");
    digest.value(m_parents);
    appendSimulationDigest(digest, m_sim);
    return digest.good();
}

bool LevelTriggers::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    digest.string("LevelTriggers");
    digest.value(m_parents);
    appendSimulationDigest(digest, saved.m_state);
    return digest.good();
}

namespace {

constexpr u32 kDefaultFlags = LevelTrigger::kStaysSolid;
constexpr u8 kTinyRadius = 0xFF;
constexpr f32 kToggleDelay = 2.0f;
constexpr f32 kHeightSpeed = 4.0f;
constexpr f32 kHeightTolerance = 0.001f;
constexpr f32 kFloorAbove = 4.0f;
constexpr f32 kFloorBelow = 10.0f;
constexpr f32 kFloorRadius = 1.0f;
constexpr f32 kFloorLift = 0.1f;
/** How far above its base an item is touched (AddItemSub's coll_offset[1] += 1). */
constexpr f32 kContactHeight = 1.0f;
/** The authored pose's floor must lie this much nearer a marker's height than the
 * initialized world's to be taken for the floor the marker was set on. */
constexpr f32 kRestPosePreference = 1.0f;
constexpr s32 kPlayerSlots = 4;

void placeHeight(WorldScene& scene, s32 object, f32 height) {
    const auto index = static_cast<usize>(object);
    Mat4 placement = scene.localTransform(index);
    // ProcessItemWobjs (80062a00) writes only the node's Y translation.
    // A separate rotator can own the same node's basis, including after it stops.
    placement[3].y = height;
    scene.setObjectTransform(index, placement);
}

u32 visitorBit(const TriggerVisitor& visitor, usize index) {
    const auto slot = visitor.party >= 0 ? static_cast<usize>(visitor.party) : index;
    return slot < static_cast<usize>(kPlayerSlots) ? 1U << slot : 0;
}

s16 paramS16(const ItemInstance& instance, usize at) {
    s16 value = 0;
    std::memcpy(&value, &instance.params[at], sizeof(value));
    return value;
}

/** A trigger kind's own flags (SetItem, items.c 6877-6891); subtypes 21 and 25-29 have no
 * names of their own. Any other subtype takes the instance's flag word with kDefaultFlags. */
struct SubtypeFlags {
    s32 subtype;
    u32 flags;
};
constexpr std::array kSubtypeFlags{
    SubtypeFlags{LevelTrigger::kMomentaryBridge, LevelTrigger::kFades},
    SubtypeFlags{21, LevelTrigger::kStaysSolid},
    SubtypeFlags{LevelTrigger::kLatchedBridge, LevelTrigger::kFades | LevelTrigger::kOpensOnce},
    SubtypeFlags{LevelTrigger::kMovementLessonPad,
                 LevelTrigger::kStaysSolid | LevelTrigger::kOpensOnce},
    SubtypeFlags{25, LevelTrigger::kDelaysWhenEmpty | LevelTrigger::kToggles},
    SubtypeFlags{26, LevelTrigger::kOpensOnce},
    SubtypeFlags{27, LevelTrigger::kDelaysWhenEmpty | LevelTrigger::kStaysSolid |
                         LevelTrigger::kToggles},
    SubtypeFlags{28, LevelTrigger::kStaysSolid | LevelTrigger::kCloses},
    SubtypeFlags{29, LevelTrigger::kStaysSolid | LevelTrigger::kOpensOnce},
};
static_assert([] {
    for (usize i = 0; i < kSubtypeFlags.size(); ++i) {
        for (usize j = i + 1; j < kSubtypeFlags.size(); ++j) {
            if (kSubtypeFlags[i].subtype == kSubtypeFlags[j].subtype) {
                return false;
            }
        }
    }
    return true;
}());

u32 flagsOf(const ItemInfo& info, const ItemInstance& instance) {
    const u32 params = static_cast<u16>(paramS16(instance, 2));
    u32 flags = params | kDefaultFlags;
    for (const SubtypeFlags& row : kSubtypeFlags) {
        if (row.subtype == info.subtype) {
            flags = row.flags;
            break;
        }
    }
    return flags | (params & ~LevelTrigger::kKindMask);
}

} // namespace

void LevelTriggers::saveLocal(LocalSnapshot& saved) const {
    saved.m_state = m_sim;
    saved.m_binding = m_checkpointBinding;
    saved.m_owner = this;
    saved.m_epoch = m_localEpoch;
}

bool LevelTriggers::canRestoreLocal(const LocalSnapshot& saved) const {
    return saved.m_owner == this && m_checkpointBinding &&
           saved.m_binding.lock() == m_checkpointBinding && saved.m_epoch == m_localEpoch;
}

bool LevelTriggers::equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const {
    return canRestoreLocal(left) && canRestoreLocal(right) && left.m_state == right.m_state;
}

bool LevelTriggers::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_state;
    return true;
}

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
        if (collision != nullptr && (info.collisionFlags & ItemInfo::kKeepsHeight) == 0) {
            support =
                initialFloors->restingFloorAt(trigger.spot, kFloorAbove, kFloorBelow, kFloorRadius);
            if (restCollision) {
                const auto rest = restCollision->restingFloorAt(trigger.spot, kFloorAbove,
                                                                kFloorBelow, kFloorRadius);
                // An explicitly floor-bound pad must not attach to a nearby still deck just
                // because its own lift is currently at the other endpoint. Any other marker
                // keeps the initialized world's floor unless the authored pose's lies a
                // unit nearer its height, the floor it was set on: the Nightmare's swinging
                // platform carries a lever authored on its deck at rest, which its first
                // frame holds ten units down and on end.
                const bool wantsTarget = (params & LevelTrigger::kOnTarget) != 0;
                const bool preferRest =
                    rest.has_value() &&
                    (!support ||
                     (wantsTarget ? !onTarget(*support) && onTarget(*rest)
                                  : std::abs(rest->y - trigger.spot.y) + kRestPosePreference <
                                        std::abs(support->y - trigger.spot.y)));
                if (preferRest) {
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
        trigger.kind = flags & LevelTrigger::kKindMask;
        trigger.radius =
            instance.params[4] == kTinyRadius ? 0.01f : 0.5f * static_cast<f32>(instance.params[4]);
        if (trigger.radius <= 0.0f) {
            trigger.radius = info.radius; // the kind's own, when the instance gives none
        }
        trigger.id = instance.params[6];
        trigger.nextId = instance.params[7];
        trigger.shootable = info.subtype == LevelTrigger::kShootableSubtype;
        trigger.height = info.height;
        trigger.movementLesson =
            info.subtype == LevelTrigger::kMovementLessonPad && trigger.target >= 0;
        // The slot is a signed byte in the data: 255 (and anything high) means none.
        const u8 slot = instance.params[5];
        trigger.sound = slot >= 0x80 ? -1 : static_cast<s32>(slot);
        m_sim.triggers.push_back(trigger);
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
            m_sim.targets.push_back(target);
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
    for (LevelTrigger& trigger : m_sim.triggers) {
        if (trigger.nextId == 0) {
            continue;
        }
        for (usize j = 0; j < m_sim.triggers.size(); ++j) {
            const LevelTrigger& other = m_sim.triggers[j];
            if (&other != &trigger && other.id == trigger.nextId &&
                (other.flags & LevelTrigger::kRequirement) == 0) {
                trigger.next = static_cast<s32>(j);
                m_sim.triggers[j].chained = true;
                break;
            }
        }
    }
    refreshEligibility(1);
}

void LevelTriggers::clear() {
    m_checkpointBinding = std::make_shared<const u8>(0);
    m_sim = {};
    m_parents.clear();
    m_localEpoch = 0;
}

void LevelTriggers::setPlayerCount(s32 players) {
    m_sim.playerCount = std::clamp(players, 0, kPlayerSlots);
    refreshEligibility(m_sim.playerCount);
}

void LevelTriggers::refreshEligibility(s32 players) {
    for (LevelTrigger& trigger : m_sim.triggers) {
        trigger.enabled = shownToParty(trigger.minPlayers, players);
        if (!trigger.enabled) {
            trigger.occupied = false;
            trigger.heldContacts = 0;
            trigger.shot = false;
        }
    }
}

void LevelTriggers::bindFigures(RenderDevice& device, const WorldLayout& layout,
                                const AssetScope& scope) {
    m_checkpointBinding = std::make_shared<const u8>(0);
    m_sim.figures.clear();
    for (const auto& trigger : m_sim.triggers) {
        const auto& instance = layout.itemInstances()[static_cast<usize>(trigger.instance)];
        const auto& info = layout.itemInfos()[static_cast<usize>(instance.info)];
        constexpr u32 kNoGeometry = 2;
        if ((instance.flags & kNoGeometry) != 0) {
            m_sim.figures.emplace_back(std::nullopt);
            continue;
        }
        std::optional<ItemFigure> figure{std::in_place};
        // Marker-only triggers have no tree. Preserve authored height: these pads
        // may sit on a moving bridge rather than on the static collision floor.
        if (!figure->place(device, scope, info.name, instance, nullptr)) {
            figure.reset();
        } else {
            figure->placeAt(trigger.placement);
        }
        m_sim.figures.push_back(std::move(figure));
    }
}

void LevelTriggers::draw(RenderDevice& device, const Mat4& clip,
                         const WorldLighting& lighting) const {
    for (usize i = 0; i < m_sim.figures.size(); ++i) {
        if (m_sim.figures[i].has_value() && m_sim.triggers[i].enabled) {
            m_sim.figures[i]->draw(device, clip, lighting);
        }
    }
}

std::vector<TriggerRefusal> LevelTriggers::takeRefusals() {
    return std::exchange(m_sim.refusals, {});
}

std::vector<TriggerOpening> LevelTriggers::takeOpenings() {
    return std::exchange(m_sim.openings, {});
}

std::vector<TriggerOpening> LevelTriggers::takeSettled() {
    return std::exchange(m_sim.settled, {});
}

std::vector<TriggerCameraCue> LevelTriggers::takeCameraCues() {
    return std::exchange(m_sim.cameraCues, {});
}

bool LevelTriggers::settled(s32 object) const {
    const Target* target = targetOf(object);
    return target == nullptr || target->settled;
}

bool LevelTriggers::movesFloor(s32 floorObject) const {
    if (floorObject < 0 || static_cast<usize>(floorObject) >= m_parents.size()) {
        return false;
    }
    const auto moving = [this](s32 object) {
        const Target* target = targetOf(object);
        return target != nullptr && (target->kind & LevelTrigger::kKeepsState) != 0 &&
               !target->settled && !(target->animated && target->ridden);
    };
    return moving(floorObject) || moving(m_parents[static_cast<usize>(floorObject)]);
}

std::vector<s32> LevelTriggers::heldTracks() const {
    std::vector<s32> held;
    for (const Target& target : m_sim.targets) {
        if (target.animated && target.ridden && !target.settled) {
            held.push_back(target.object);
        }
    }
    return held;
}

TriggerOpening LevelTriggers::openingOf(const Target& target, WorldAnimator::Finish finish) {
    return TriggerOpening{target.object,
                          target.spot,
                          (target.kind & LevelTrigger::kFades) != 0,
                          finish == WorldAnimator::Finish::AtOnce,
                          target.sound,
                          target.subtype,
                          !target.open};
}

LevelTriggers::Target* LevelTriggers::targetOf(s32 object) {
    for (Target& target : m_sim.targets) {
        if (target.object == object) {
            return &target;
        }
    }
    return nullptr;
}

const LevelTriggers::Target* LevelTriggers::targetOf(s32 object) const {
    for (const Target& target : m_sim.targets) {
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
        const s32 tier = std::clamp(trigger.id - LevelTrigger::kIconTierBase, 0, 2);
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

bool LevelTriggers::openTarget(Target& target, bool open, WorldAnimator::Finish finish,
                               WorldAnimator& animator, WorldScene& scene,
                               WorldCollision* collision) {
    const bool atOnce = finish == WorldAnimator::Finish::AtOnce;
    if ((!open && target.forced) || (target.open == open && (!atOnce || target.settled))) {
        return false;
    }
    target.open = open;
    target.returning = false;
    target.settled = atOnce; // only an opening before the party is worth reporting done
    if (atOnce && collision != nullptr) {
        collision->setFloorExitBlocked(target.object, false);
        collision->setPlayerPassThrough(target.object, false);
    }
    if (target.animated) {
        if (!target.forced && (target.kind & LevelTrigger::kOscillates) != 0 && !atOnce) {
            animator.cycle(target.object, open);
        } else {
            animator.fire(target.object, open, finish);
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
        placeHeight(scene, target.object, target.origin.y + target.height);
    }
    return true;
}

void LevelTriggers::fire(usize index, bool active, WorldAnimator::Finish finish,
                         WorldAnimator& animator, WorldScene& scene, WorldCollision* collision) {
    usize followed = 0;
    for (auto at = static_cast<s32>(index); at >= 0 && followed++ < m_sim.triggers.size();
         at = m_sim.triggers[static_cast<usize>(at)].next) {
        fireNode(static_cast<usize>(at), active, finish, animator, scene, collision);
    }
}

void LevelTriggers::fireNode(usize index, bool active, WorldAnimator::Finish finish,
                             WorldAnimator& animator, WorldScene& scene,
                             WorldCollision* collision) {
    LevelTrigger& trigger = m_sim.triggers[index];
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
        switch (trigger.latch()) {
        case TriggerLatch::Closes:
            if (contact && target->settled) {
                open = false;
            }
            trigger.fired = !open;
            break;
        case TriggerLatch::OpensOnce:
            if (contact && target->settled) {
                open = true;
            }
            trigger.fired = open;
            break;
        case TriggerLatch::Toggles:
            if (contact && !target->settled) {
                trigger.toggleCooldown = kToggleDelay;
            } else if (contact && trigger.toggleCooldown <= 0) {
                open = !open;
                trigger.toggleCooldown = kToggleDelay;
            } else if (!contact && (trigger.flags & LevelTrigger::kDelaysWhenEmpty) != 0) {
                // Empty-pad delay is one second per additional participant.
                trigger.toggleCooldown = m_sim.emptyToggleDelay;
            }
            trigger.fired = contact;
            break;
        case TriggerLatch::Momentary:
            if (contact) {
                open = true;
            }
            trigger.fired = contact;
            break;
        }
        if (openTarget(*target, open, finish, animator, scene, collision)) {
            target->spot = trigger.spot;
            target->sound = trigger.sound;
            m_sim.openings.push_back(openingOf(*target, finish));
        }
    } else if (contact || (trigger.flags & LevelTrigger::kToggles) != 0) {
        trigger.fired = contact;
    }
    if (trigger.fired && !wasFired && contact && finish == WorldAnimator::Finish::Animated) {
        m_sim.cameraCues.push_back(
            {trigger.id, trigger.target, (trigger.flags & LevelTrigger::kShakesCamera) != 0});
        if ((trigger.flags & LevelTrigger::kWakesStatue) != 0) {
            m_sim.wakes.push_back(trigger.spot);
        }
    }
    if (trigger.fired != wasFired && index < m_sim.figures.size() && m_sim.figures[index]) {
        auto& figure = *m_sim.figures[index];
        const s32 onSequence = finish == WorldAnimator::Finish::AtOnce ? 2 : 1;
        figure.play(trigger.fired ? onSequence : 3, ItemFigure::Playback::Once);
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
    // fn_8005F0F4 measures the height from the body's centre (effectpos, the feet plus
    // col_height) to the item's contact point, a unit above its base (AddItemSub adds one
    // to every kind's collision offset; no trigger kind carries one of its own). So a pad
    // sunk five below the feet, on a lift yet to rise, is out of reach while one five
    // above the feet is not.
    const f32 half = visitor.height * 0.5f;
    const f32 rise = visitor.position.y + half - (trigger.spot.y + kContactHeight);
    return away.x * away.x + away.z * away.z <= reach * reach &&
           std::abs(rise) <= trigger.height + half;
}

void LevelTriggers::collectContacts(usize index, u32 mask, std::span<u32> contacts) const {
    // Contact identities travel through the entire chain. Each node's party-size,
    // floor and whole-party requirements govern that node, not later successors.
    usize followed = 0;
    for (auto at = static_cast<s32>(index); at >= 0 && followed++ < m_sim.triggers.size();
         at = m_sim.triggers[static_cast<usize>(at)].next) {
        contacts[static_cast<usize>(at)] |= mask;
    }
}

void LevelTriggers::shoot(usize index) {
    if (index < m_sim.triggers.size() && m_sim.triggers[index].shootable) {
        m_sim.triggers[index].shot = true;
    }
}

bool LevelTriggers::press(usize index) {
    if (index >= m_sim.triggers.size() || !m_sim.triggers[index].enabled ||
        m_sim.triggers[index].forced) {
        return false;
    }
    m_sim.triggers[index].shot = true;
    return true;
}

void LevelTriggers::openMet(std::span<const TriggerVisitor> visitors, WorldAnimator& animator,
                            WorldScene& scene, WorldCollision* collision) {
    refreshEligibility(m_sim.playerCount >= 0 ? m_sim.playerCount
                                              : static_cast<s32>(visitors.size()));
    for (usize i = 0; i < m_sim.triggers.size(); ++i) {
        const LevelTrigger& trigger = m_sim.triggers[i];
        if (trigger.enabled && (trigger.flags & LevelTrigger::kRequirement) != 0 &&
            qualifies(trigger, visitors)) {
            fire(i, true, WorldAnimator::Finish::AtOnce, animator, scene, collision);
        }
    }
    placeTargets(scene, collision);
}

void LevelTriggers::placeTargets(WorldScene& scene, WorldCollision* collision) {
    for (Target& target : m_sim.targets) {
        if ((target.kind & LevelTrigger::kFades) != 0) {
            target.alpha = fadesAway(target) ? 0.0f : 1.0f;
            applyAlpha(target, scene, collision);
        }
        if (!target.animated && (target.kind & LevelTrigger::kFades) == 0) {
            placeHeight(scene, target.object, target.origin.y + target.height);
        }
    }
}

void LevelTriggers::openAtOnce(std::span<const s32> ids, WorldAnimator& animator, WorldScene& scene,
                               WorldCollision* collision) {
    for (const LevelTrigger& trigger : m_sim.triggers) {
        if (std::ranges::find(ids, trigger.id) == ids.end()) {
            continue;
        }
        Target* target = targetOf(trigger.target);
        if (target != nullptr &&
            openTarget(*target, true, WorldAnimator::Finish::AtOnce, animator, scene, collision)) {
            target->spot = trigger.spot;
            target->sound = trigger.sound;
            m_sim.openings.push_back(openingOf(*target, WorldAnimator::Finish::AtOnce));
        }
    }
}

void LevelTriggers::activate(s32 id, WorldAnimator::Finish finish, WorldAnimator& animator,
                             WorldScene& scene, WorldCollision* collision) {
    for (usize i = 0; i < m_sim.triggers.size(); ++i) {
        if (m_sim.triggers[i].id != id) {
            continue;
        }
        s32 at = static_cast<s32>(i);
        for (usize guard = 0; at >= 0 && guard < m_sim.triggers.size(); ++guard) {
            auto& trigger = m_sim.triggers[static_cast<usize>(at)];
            trigger.forced = true;
            trigger.fired = true;
            if (Target* target = targetOf(trigger.target)) {
                target->forced = true;
                if (openTarget(*target, true, finish, animator, scene, collision)) {
                    target->spot = trigger.spot;
                    target->sound = trigger.sound;
                    m_sim.openings.push_back(openingOf(*target, finish));
                }
            }
            at = trigger.next;
        }
    }
}

void LevelTriggers::update(f32 seconds, std::span<const TriggerVisitor> visitors,
                           WorldAnimator& animator, WorldScene& scene, WorldCollision* collision,
                           std::span<const s32> occupiedFloors) {
    refreshEligibility(m_sim.playerCount >= 0 ? m_sim.playerCount
                                              : static_cast<s32>(visitors.size()));
    m_sim.emptyToggleDelay = visitors.empty() ? 0.0f : static_cast<f32>(visitors.size() - 1);
    u32 standingMask = 0;
    for (usize i = 0; i < visitors.size(); ++i) {
        standingMask |= visitorBit(visitors[i], i);
    }
    std::vector<u32> contacts(m_sim.triggers.size(), 0);
    for (Target& target : m_sim.targets) {
        // A platform that opens to players as it moves cannot move under one standing on it:
        // it waits for them to step off, solid where it stands. did_generate (0x80063854)
        // counts a player whose floor (+0x8C4) is the target itself, so a lift whose deck is a
        // separate piece carries the deck's riders on.
        target.ridden = target.holdsForRiders() &&
                        std::ranges::any_of(visitors, [&](const TriggerVisitor& visitor) {
                            return visitor.floorObject == target.object;
                        });
        // ProcessItemWobjs (GC 0x80062A00) keeps a momentary fade's activation
        // while did_generate finds a player or live enemy on it; here anyone on the
        // bridge's subtree keeps it. Retain an already revealed bridge only: standing
        // over an absent one must not activate its button, chain, camera or whole-party
        // requirement.
        const auto supported = [&](s32 floor) {
            return floor >= 0 && std::ranges::find(target.subtree, static_cast<usize>(floor)) !=
                                     target.subtree.end();
        };
        const bool momentaryBridge = LevelTrigger::momentaryFade(target.kind);
        target.pressed = target.open && momentaryBridge &&
                         (std::ranges::any_of(visitors,
                                              [&](const TriggerVisitor& visitor) {
                                                  return supported(visitor.floorObject);
                                              }) ||
                          std::ranges::any_of(occupiedFloors, supported));
        target.wholePartyReady = false;
    }
    for (usize i = 0; i < m_sim.triggers.size(); ++i) {
        LevelTrigger& trigger = m_sim.triggers[i];
        if (collision != nullptr && trigger.floor >= 0) {
            if (const auto floor = collision->objectTransform(trigger.floor)) {
                const Mat4 placement = *floor * trigger.localPlacement;
                trigger.placement = placement;
                trigger.spot = Vec3{placement[3]};
                if (i < m_sim.figures.size() && m_sim.figures[i].has_value()) {
                    m_sim.figures[i]->placeAt(placement);
                }
            }
        }
    }
    for (usize i = 0; i < m_sim.triggers.size(); ++i) {
        LevelTrigger& trigger = m_sim.triggers[i];
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
                m_sim.lessons.push_back(
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
            m_sim.refusals.push_back(
                TriggerRefusal{static_cast<s32>(i), trigger.id, trigger.needsCrystals()});
            trigger.refusalCooldown = kRefusalCooldown;
        }
    }
    // Evaluate each node once, after every contact root has supplied its visitors.
    // An unavailable linked node is skipped locally, not a break in the chain.
    for (usize i = 0; i < m_sim.triggers.size(); ++i) {
        auto& trigger = m_sim.triggers[i];
        if (trigger.forced || !trigger.enabled) {
            continue;
        }
        u32 mask = contacts[i];
        if (m_sim.cameraHeld || (trigger.flags & LevelTrigger::kKeepContact) != 0) {
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
        fireNode(i, mask != 0, WorldAnimator::Finish::Animated, animator, scene, collision);
    }
    // An unoccupied pad must not cancel another pad's contact, or reset a target
    // registered as a latch by its first switch.
    for (Target& target : m_sim.targets) {
        if (!target.forced && (target.kind & LevelTrigger::kLatches) == 0 && !target.pressed &&
            openTarget(target, false, WorldAnimator::Finish::Animated, animator, scene,
                       collision)) {
            m_sim.openings.push_back(openingOf(target, WorldAnimator::Finish::Animated));
        }
    }
    stepMotion(seconds, scene, collision, true);
    for (usize i = 0; i < m_sim.figures.size(); ++i) {
        if (!m_sim.figures[i] || !m_sim.triggers[i].enabled) {
            continue;
        }
        auto& figure = *m_sim.figures[i];
        figure.update(seconds);
        if (m_sim.triggers[i].fired && figure.sequence() == 1 && figure.finished()) {
            figure.play(2, ItemFigure::Playback::Loop);
        } else if (!m_sim.triggers[i].fired && figure.sequence() == 3 && figure.finished()) {
            figure.play(0, ItemFigure::Playback::Loop);
        }
    }
    finishMotion(animator, collision, true);
}

void LevelTriggers::stepMotion(f32 seconds, WorldScene& scene, WorldCollision* collision,
                               bool report) {
    // Fields thin out a step a game frame.
    m_sim.frameRemainder += seconds * kFrameRate;
    const f32 frames = std::floor(m_sim.frameRemainder);
    m_sim.frameRemainder -= frames;
    for (Target& target : m_sim.targets) {
        if (!target.animated && (target.kind & LevelTrigger::kFades) == 0) {
            const bool held = !target.settled && target.ridden;
            if (!target.settled && !held) {
                // GameCube items.c ProcessItemWobjs: 4.0 * gClockFrameStep.
                const f32 goal =
                    target.open && !target.returning ? target.openHeight : target.closedHeight;
                const f32 distance = goal - target.height;
                const f32 step = kHeightSpeed * seconds;
                target.height += std::clamp(distance, -step, step);
                if (std::abs(goal - target.height) <= kHeightTolerance) {
                    target.height = goal;
                    target.settled = true;
                    // Equal endpoints still finish the opening sound's lifetime.
                    if (report) {
                        m_sim.settled.push_back(openingOf(target, WorldAnimator::Finish::Animated));
                    }
                    if (!target.forced && target.open &&
                        (target.kind & LevelTrigger::kOscillates) != 0 &&
                        target.openHeight != target.closedHeight) {
                        target.returning = !target.returning;
                        target.settled = false;
                    }
                }
            }
            if (collision != nullptr) {
                collision->setPlayerPassThrough(target.object, target.opensToPlayers());
            }
            if (held) {
                continue; // held under its riders, as solid as it stands
            }
            placeHeight(scene, target.object, target.origin.y + target.height);
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
            if (report) {
                m_sim.settled.push_back(openingOf(target, WorldAnimator::Finish::Animated));
            }
        }
    }
}

void LevelTriggers::finishMotion(const WorldAnimator& animator, WorldCollision* collision,
                                 bool report) {
    // An animated target is done once its animation has run to the end.
    for (Target& target : m_sim.targets) {
        if (!target.animated) {
            continue;
        }
        if (const auto track = animator.trackOf(target.object);
            !target.settled && track.has_value() && animator.finished(*track)) {
            target.settled = true;
            if (report) {
                m_sim.settled.push_back(openingOf(target, WorldAnimator::Finish::Animated));
            }
        }
        if (collision != nullptr) {
            collision->setPlayerPassThrough(target.object, target.opensToPlayers());
        }
    }
    if (collision != nullptr) {
        for (const Target& target : m_sim.targets) {
            // PlayerCollideFloor keeps a rider on its floor while the flags of that floor and its
            // parents (WorldObjGetAllFlags) hold 0x20000000 with 0x08000000 or 0x04000000
            // (0x80088128-0x80088174). ProcessItemWobjs' tail sets 0x20000000 on a target that
            // moved this pass (0x80062F38-0x80062F64), and a height step (0x80062F08) or a
            // playing track (DoWorldAnimSub, 0x800A8A54-0x800A8A68) 0x08000000: a lift keeps its
            // riders for its whole travel, whoever stands in its pads. A fade's step sets no
            // 0x08000000, so a fade keeps them only while the whole party stands in its pads
            // (0x04000000, 0x800619D4-0x80061A1C). A target its riders hold skips the tail.
            const bool fades = (target.kind & LevelTrigger::kFades) != 0;
            collision->setFloorExitBlocked(target.object, !target.settled && !target.ridden &&
                                                              (!fades || target.wholePartyReady));
        }
    }
}

bool LevelTriggers::Target::holdsForRiders() const {
    return (kind & (LevelTrigger::kFades | LevelTrigger::kStaysSolid)) == 0;
}

bool LevelTriggers::Target::opensToPlayers() const {
    // ProcessItemWobjs' tail marks a target that moved this pass (0x80062F38-0x80062F4C);
    // one its riders hold skips the tail and stays solid.
    return holdsForRiders() && !settled && !ridden;
}

bool LevelTriggers::Motion::Target::valid() const {
    return object >= 0 && object < 65535 && std::isfinite(alpha) && alpha >= 0 && alpha <= 1 &&
           std::isfinite(height) && std::abs(height) <= 1'000'000;
}

bool LevelTriggers::Motion::valid() const {
    if (!std::isfinite(frameRemainder) || frameRemainder < 0 || frameRemainder >= 1 ||
        targets.size() > kMaxTargets) {
        return false;
    }
    std::unordered_set<s32> objects;
    for (const auto& target : targets) {
        if (!target.valid() || !objects.insert(target.object).second) {
            return false;
        }
    }
    return true;
}

LevelTriggers::Motion LevelTriggers::motion() const {
    Motion result;
    result.frameRemainder = m_sim.frameRemainder;
    result.targets.reserve(m_sim.targets.size());
    for (const auto& target : m_sim.targets) {
        result.targets.push_back({target.object, target.open, target.settled, target.pressed,
                                  target.wholePartyReady, target.returning, target.forced,
                                  target.alpha, target.height});
    }
    return result;
}

bool LevelTriggers::acceptsMotion(const Motion& state) const {
    if (!state.valid() || state.targets.size() != m_sim.targets.size()) {
        return false;
    }
    for (usize index = 0; index < m_sim.targets.size(); ++index) {
        const auto& target = m_sim.targets[index];
        const auto& cursor = state.targets[index];
        if (cursor.object != target.object || !std::isfinite(target.closedHeight) ||
            !std::isfinite(target.openHeight) ||
            cursor.height < std::min(target.closedHeight, target.openHeight) - kHeightTolerance ||
            cursor.height > std::max(target.closedHeight, target.openHeight) + kHeightTolerance) {
            return false;
        }
    }
    return true;
}

bool LevelTriggers::restoreMotion(const Motion& state) {
    if (!acceptsMotion(state)) {
        return false;
    }
    for (usize index = 0; index < m_sim.targets.size(); ++index) {
        auto& target = m_sim.targets[index];
        const auto& cursor = state.targets[index];
        target.open = cursor.open;
        target.settled = cursor.settled;
        target.pressed = cursor.pressed;
        target.wholePartyReady = cursor.wholePartyReady;
        target.returning = cursor.returning;
        target.forced = cursor.forced;
        target.alpha = cursor.alpha;
        target.height = cursor.height;
    }
    m_sim.frameRemainder = state.frameRemainder;
    ++m_localEpoch;
    return true;
}

bool LevelTriggers::advanceMotion(f32 seconds, const WorldAnimator& animator, WorldScene& scene,
                                  WorldCollision* collision) {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 0.25f) {
        return false;
    }
    stepMotion(seconds, scene, collision, false);
    finishMotion(animator, collision, false);
    return true;
}

} // namespace gdl::game
