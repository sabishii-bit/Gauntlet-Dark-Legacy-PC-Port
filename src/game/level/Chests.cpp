#include "game/level/Chests.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/NativeStateDigest.h"

#include "game/level/ItemSupport.h"
#include "game/level/NativeStateResources.h"

namespace gdl::game {
namespace {
/** do_see_thru sets the overlay node's scale to 0.65, so everything under it, its particles
 * and facing parts included, rides the shrunken item. */
constexpr f32 kPreviewScale = 0.65f;
} // namespace

namespace {

s16 paramS16(const ItemInstance& instance, usize at) {
    s16 value = 0;
    std::memcpy(&value, &instance.params[at], sizeof(value));
    return value;
}

} // namespace

bool Chests::appendSimulationDigest(NativeStateDigest& digest, const Simulation& state) const {
    digest.string("Chests.native.1");
    // These immutable binding inputs remain fixed until clear/bind invalidates
    // history. Mutable collision values are visited by the enclosing world owner.
    digest.resource(m_layout);
    digest.resource(m_collision);
    digest.wide(m_infos.size());
    for (const auto& info : m_infos) {
        digest.fields<19>(info);
    }
    digest.fields<3>(state);
    return digest.good();
}

bool Chests::appendNativeDigest(NativeStateDigest& digest) const {
    return appendSimulationDigest(digest, m_sim);
}

bool Chests::appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    return appendSimulationDigest(digest, saved.m_state);
}

bool Chests::registerNativeResources(NativeStateResources& resources) const {
    for (const auto& chest : m_sim.chests) {
        if (!chest.figure.registerNativeResources(resources) ||
            !chest.preview.registerNativeResources(resources)) {
            return false;
        }
    }
    return resources.good();
}

void Chests::saveLocal(LocalSnapshot& saved) const {
    saved.m_state = m_sim;
    saved.m_binding = m_checkpointBinding;
    saved.m_owner = this;
}

bool Chests::canRestoreLocal(const LocalSnapshot& saved) const {
    return saved.m_owner == this && m_checkpointBinding &&
           saved.m_binding.lock() == m_checkpointBinding;
}

bool Chests::equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const {
    return canRestoreLocal(left) && canRestoreLocal(right) && left.m_state == right.m_state;
}

bool Chests::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_state;
    ++m_checkpointGeneration;
    return true;
}

s32 Chests::contentsRecord(std::span<const ItemInfo> infos, s32 record, s32 count) {
    constexpr s32 kKey = 2;
    if (record < 0 || static_cast<usize>(record) >= infos.size() || count <= 1 ||
        infos[static_cast<usize>(record)].type != ItemInfo::kPowerup ||
        infos[static_cast<usize>(record)].subtype != kKey) {
        return record;
    }
    for (usize i = 0; i < infos.size(); ++i) {
        if (infos[i].type == ItemInfo::kPowerup && infos[i].subtype == kKey &&
            infos[i].name == "KEYRING") {
            return static_cast<s32>(i);
        }
    }
    return record;
}

s32 Chests::resolveContents(std::span<const ItemInfo> infos, s32 record, usize itemIndex,
                            u32& seed) {
    // A list may name a list; each pick moves the seed on.
    for (s32 hops = 0; hops < 8; ++hops) {
        if (record < 0 || static_cast<usize>(record) >= infos.size()) {
            return -1;
        }
        const ItemInfo& info = infos[static_cast<usize>(record)];
        if (info.type != ItemInfo::kChoiceList) {
            return record;
        }
        if (info.choices.empty()) {
            return -1;
        }
        const auto pick =
            ((seed >> 5U) + static_cast<u32>(itemIndex)) % static_cast<u32>(info.choices.size());
        seed += static_cast<u32>(kSeedStep);
        record = info.choices[pick];
    }
    return -1;
}

bool Chests::bind(RenderDevice& device, const WorldLayout& layout, const AssetScope& scope,
                  const WorldCollision* collision) {
    clear();
    m_collision = collision;
    m_layout = &layout;
    const auto restCollision = itemSupportWorld(layout, collision);
    m_infos = layout.itemInfos();
    const std::vector<ItemInstance>& instances = layout.itemInstances();
    for (usize index = 0; index < instances.size(); ++index) {
        const ItemInstance& instance = instances[index];
        if (instance.info < 0 || static_cast<usize>(instance.info) >= m_infos.size()) {
            continue;
        }
        const ItemInfo& info = m_infos[static_cast<usize>(instance.info)];
        if (info.type != ItemInfo::kContainer || info.subtype == kBarrel) {
            continue;
        }
        Chest chest;
        chest.instance = static_cast<s32>(index);
        chest.info = instance.info;
        chest.subtype = info.subtype;
        chest.contents = paramS16(instance, 0);
        chest.count = paramS16(instance, 4);
        chest.minPlayers = instance.minPlayers;
        chest.locked = (static_cast<u32>(info.activeType) & kLocked) != 0;
        const std::string& name = instance.name.empty() ? info.name : instance.name;
        if (!chest.figure.place(device, scope, name, instance,
                                (info.collisionFlags & ItemInfo::kKeepsHeight) != 0 ? nullptr
                                                                                    : collision)) {
            log::warn("Chests: no figure {} in the item archive", name);
        }
        const auto* support =
            itemSupportAt(instance.position, collision, restCollision ? &*restCollision : nullptr);
        if (support != nullptr && (info.collisionFlags & ItemInfo::kKeepsHeight) == 0) {
            constexpr f32 kAbove = 4.0f;
            constexpr f32 kBelow = 10.0f;
            if (const auto floor = support->restingFloorAt(instance.position, kAbove, kBelow, 1)) {
                if (const auto placement = support->objectTransform(floor->object)) {
                    Vec3 position = instance.position;
                    position.y = floor->y + ItemFigure::kFloorLift;
                    chest.floor =
                        Chest::Floor{floor->object, glm::inverse(*placement) *
                                                        itemPlacement(position, instance.rotation)};
                }
            }
        }
        chest.box = chest.figure.obstacle(info);
        m_sim.chests.push_back(std::move(chest));
    }
    syncFloors();
    return !m_sim.chests.empty();
}

void Chests::syncFloors() {
    if (m_collision == nullptr) {
        return;
    }
    for (auto& entry : m_sim.chests) {
        Chest& chest = entry;
        if (!chest.floor || chest.gone) {
            continue;
        }
        const auto placement = m_collision->objectTransform(chest.floor->object);
        // A moving floor can turn collision off without deleting its scene node.
        if (!placement) {
            chest.floor.reset();
            continue;
        }
        const Mat4 transform = *placement * chest.floor->local;
        chest.figure.placeAt(transform);
        chest.preview.placeAt(glm::scale(transform, Vec3{kPreviewScale}));
        chest.box = chest.figure.obstacle(m_infos[static_cast<usize>(chest.info)]);
    }
}

void Chests::clear() {
    m_checkpointBinding = std::make_shared<const u8>(0);
    m_sim.chests.clear();
    m_infos.clear();
    m_sim.seed = 0;
    m_sim.seedRemainder = 0.0f;
    m_collision = nullptr;
    m_layout = nullptr;
}

void Chests::setPlayerCount(s32 players) {
    for (auto& chest : m_sim.chests) {
        chest.shown = shownToParty(chest.minPlayers, players);
    }
}

std::vector<ChestEvent> Chests::update(f32 seconds, std::span<const ChestVisitor> party) {
    syncFloors();
    // The shared seed walks on with the frames (sItemRandSeed + 1 a frame), so a random
    // chest cycles through its choices until it is opened, as the X-ray overlay shows.
    m_sim.seedRemainder += std::max(seconds, 0.0f) * kSeedTicksPerSecond;
    const f32 seedTicks = std::floor(m_sim.seedRemainder);
    m_sim.seedRemainder -= seedTicks;
    m_sim.seed += static_cast<u32>(seedTicks);
    std::vector<ChestEvent> events;
    std::vector<s32> goldCapacity;
    goldCapacity.reserve(party.size());
    for (const auto& visitor : party) {
        goldCapacity.push_back(visitor.goldCapacity);
    }
    for (usize index = 0; index < m_sim.chests.size(); ++index) {
        Chest& chest = m_sim.chests[index];
        if (!chest.shown || chest.gone) {
            continue;
        }
        if (chest.collectedSeconds) {
            *chest.collectedSeconds -= seconds;
            if (*chest.collectedSeconds <= 0) {
                chest.gone = true;
            }
            continue;
        }
        chest.figure.update(seconds);
        if (chest.revealed) {
            chest.preview.update(seconds); // the X-ray overlay plays the pickup's own cycle
        }
        chest.refusalLeft = std::max(chest.refusalLeft - seconds, 0.0f);
        if (chest.wobble > 0.0f && chest.state == kShut) {
            rock(chest, seconds);
        }
        if (chest.state == kShut) {
            for (usize v = 0; v < party.size(); ++v) {
                if (!chest.box.touchedBy(party[v].position, party[v].radius)) {
                    continue;
                }
                ChestEvent event;
                event.chest = index;
                event.visitor = v;
                event.position = chest.figure.position();
                if (chest.locked && party[v].keys <= 0) {
                    if (chest.refusalLeft <= 0.0f) {
                        chest.refusalLeft = kRefusalSeconds;
                        event.kind = ChestEvent::Kind::Refused;
                        events.push_back(event);
                    }
                    continue;
                }
                chest.state = kOpening;
                chest.opener = static_cast<s32>(v);
                chest.figure.play(kOpening, false);
                event.kind = ChestEvent::Kind::Unlocked;
                chest.contents =
                    contentsRecord(m_infos,
                                   resolveContents(m_infos, chest.contents,
                                                   static_cast<usize>(chest.instance), m_sim.seed),
                                   chest.count);
                if (chest.subtype != kTrappedChest && chest.subtype != kGoldChest) {
                    event.contents = chest.contents;
                }
                events.push_back(event);
                break;
            }
        } else if (chest.state == kOpening && chest.figure.finished()) {
            chest.state = kOpen;
            chest.figure.play(kOpen, true);
            ChestEvent event;
            event.kind = ChestEvent::Kind::Opened;
            event.chest = index;
            event.visitor = static_cast<usize>(std::max(chest.opener, 0));
            event.position = chest.figure.position();
            event.explodes = chest.subtype == kTrappedChest;
            const s32 inside = chest.contents;
            if (!event.explodes && inside >= 0 && chest.subtype != kGoldChest) {
                event.contents = inside;
            }
            events.push_back(event);
        } else if (chest.state == kOpen && chest.subtype == kGoldChest && chest.contents >= 0) {
            // ItemTouch (8005D730) grants the touching player gold, not necessarily
            // the opener, then retires the container eight ticks later.
            for (usize visitor = 0; visitor < party.size(); ++visitor) {
                if (goldCapacity[visitor] > 0 &&
                    chest.box.touchedBy(party[visitor].position, party[visitor].radius)) {
                    ChestEvent event;
                    event.kind = ChestEvent::Kind::Collected;
                    event.chest = index;
                    event.visitor = visitor;
                    event.position = chest.figure.position();
                    event.gold = m_infos[static_cast<usize>(chest.contents)].value;
                    // Events apply after this pass; reserve capacity now so a full
                    // wallet cannot consume another touching chest in the same update.
                    goldCapacity[visitor] = std::max(0, goldCapacity[visitor] - event.gold);
                    chest.collectedSeconds = kCollectedSeconds;
                    events.push_back(event);
                    break;
                }
            }
        }
    }
    return events;
}

bool Chests::transmute(usize index, s32 record, f32 power) {
    if (index >= m_sim.chests.size()) {
        return false;
    }
    Chest& chest = m_sim.chests[index];
    if (!chest.shown || chest.gone || chest.state != kShut) {
        return false;
    }
    chest.contents = record;
    chest.previewContents = -1; // an X-ray overlay shows the new contents at once
    chest.wobble = std::trunc(kWobblePerPower * power);
    for (auto& other : m_sim.chests) {
        if (other.info == chest.info) {
            other.subtype = kTransmuted;
        }
    }
    return true;
}

/** The rock of the item loop (fn_800606FC): a square wave of sixteen ticks, pitched three
 * degrees and turned four either way a quarter of it apart, until the count runs out. */
void Chests::rock(Chest& chest, f32 seconds) {
    chest.wobble = std::max(chest.wobble - seconds * kTicksPerSecond, 0.0f);
    if (chest.wobble <= 0.0f) {
        chest.figure.rest();
        return;
    }
    constexpr s32 kHalfWave = 8;
    constexpr s32 kQuarterWave = 4;
    const auto ticks = static_cast<s32>(chest.wobble);
    const f32 pitch = ((ticks + kQuarterWave) & kHalfWave) != 0 ? -kRockPitch : kRockPitch;
    const f32 yaw = (ticks & kHalfWave) != 0 ? kRockYaw : -kRockYaw;
    chest.figure.tilt(pitch, yaw);
}

bool Chests::changeContents(usize index, s32 record, RenderDevice& device,
                            const WorldLayout& layout, const AssetScope& scope,
                            const WorldCollision* collision, std::string_view figure) {
    if (index >= m_sim.chests.size()) {
        return false;
    }
    Chest& chest = m_sim.chests[index];
    if (!chest.shown || chest.gone || chest.state != kShut) {
        return false;
    }
    chest.contents = record;
    chest.previewContents = -1; // an X-ray overlay shows the new contents at once
    if (!figure.empty() && scope.findTree(figure).has_value()) {
        const Mat4 placement = chest.figure.transform();
        const ItemInstance& instance = layout.itemInstances()[static_cast<usize>(chest.instance)];
        chest.figure.place(device, scope, figure, instance, collision);
        chest.figure.placeAt(placement);
        chest.figure.play(kShut, true);
    }
    return true;
}

void Chests::hold(usize chest, s32 item) {
    if (chest < m_sim.chests.size()) {
        m_sim.chests[chest].held = item;
    }
}

usize Chests::updateXray(RenderDevice& device, const AssetScope& scope, ItemArchive& powerups,
                         std::span<const ChestVisitor> party) {
    std::vector<bool> selected(m_sim.chests.size(), false);
    for (const auto& visitor : party) {
        if (!visitor.xray) {
            continue;
        }
        f32 best = 10.0f * 10.0f;
        usize closest = m_sim.chests.size();
        for (usize i = 0; i < m_sim.chests.size(); ++i) {
            const auto& chest = m_sim.chests[i];
            // ClosestChest checks visibility and action, not the container subtype:
            // X-Ray also warns about a bomb inside a trapped chest.
            if (!chest.shown || chest.gone || chest.state != kShut) {
                continue;
            }
            u32 seed = m_sim.seed;
            const s32 inside =
                resolveContents(m_infos, chest.contents, static_cast<usize>(chest.instance), seed);
            if (inside < 0 ||
                (m_infos[static_cast<usize>(inside)].type != ItemInfo::kPowerup &&
                 m_infos[static_cast<usize>(inside)].type != ItemInfo::kPlacedEnemy)) {
                continue;
            }
            const Vec3 delta = chest.figure.position() - visitor.position;
            const f32 distance = glm::dot(delta, delta);
            if (distance < best) {
                closest = i;
                best = distance;
            }
        }
        if (closest < selected.size()) {
            selected[closest] = true;
        }
    }
    usize fresh = 0;
    for (usize i = 0; i < m_sim.chests.size(); ++i) {
        auto& chest = m_sim.chests[i];
        if (selected[i]) {
            fresh += chest.revealed ? 0U : 1U;
            u32 seed = m_sim.seed; // Looking must not change what opening a random chest produces.
            const s32 inside = contentsRecord(
                m_infos,
                resolveContents(m_infos, chest.contents, static_cast<usize>(chest.instance), seed),
                chest.count);
            if (inside != chest.previewContents) {
                chest.previewContents = inside;
                const auto& record = m_infos[static_cast<usize>(inside)];
                std::string_view name = record.name;
                if (record.type == ItemInfo::kPlacedEnemy) {
                    name = "DEATH_ICON";
                } else if (record.subtype == ItemInfo::kKey && chest.count > 1) {
                    name = "KEYRING";
                }
                AssetScope previewArt = scope;
                previewArt.add(powerups);
                ItemInstance instance;
                instance.position = chest.figure.position();
                instance.rotation.y = -chest.figure.yaw();
                // The overlay plays the pickup's own cycle from here: held at its first
                // frame, the growth potion shows at a tenth of its size and a breath icon
                // before its fade-in.
                chest.preview.place(device, previewArt, name, instance, nullptr);
                chest.preview.placeAt(glm::scale(chest.preview.transform(), Vec3{kPreviewScale}));
            }
        }
        chest.revealed = selected[i];
    }
    return fresh;
}

s32 Chests::holdingTouchedBy(const ChestVisitor& visitor) const {
    for (usize index = 0; index < m_sim.chests.size(); ++index) {
        const Chest& chest = m_sim.chests[index];
        if (chest.shown && !chest.gone && chest.state == kOpen && chest.held >= 0 &&
            chest.box.touchedBy(visitor.position, visitor.radius)) {
            return static_cast<s32>(index);
        }
    }
    return -1;
}

void Chests::remove(usize chest) {
    if (chest < m_sim.chests.size()) {
        m_sim.chests[chest].gone = true;
        m_sim.chests[chest].held = -1;
    }
}

std::vector<Obstacle> Chests::obstacles() const {
    std::vector<Obstacle> boxes;
    for (const auto& chest : m_sim.chests) {
        // ItemTouch (8005D730) leaves an open pickup chest passable even when a
        // full-health player cannot take its food. Open gold chests likewise stay
        // reachable without trapping someone whose wallet cannot take their gold.
        const bool holdsPickup =
            chest.state == kOpen && (chest.held >= 0 || chest.subtype == kGoldChest);
        if (chest.shown && !chest.gone && !holdsPickup) {
            boxes.push_back(chest.box);
        }
    }
    return boxes;
}

void Chests::capturePresentation() {
    for (auto& chest : m_sim.chests) {
        chest.figure.capturePresentation();
        chest.preview.capturePresentation();
    }
}

void Chests::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                  const CameraFrame* camera, f32 presentationAlpha) const {
    for (const auto& chest : m_sim.chests) {
        if (chest.shown && !chest.gone) {
            if (chest.revealed) {
                // Retail MBTreeSetAlpha(0xC0) is transparency, not opacity. The overlay is
                // drawn as a pickup is: its solids, then its alpha-textured and facing parts
                // composited smoothly rather than by coverage.
                for (const auto pass : {TreeModel::Pass::Opaque, TreeModel::Pass::Blended}) {
                    chest.preview.draw(device, clip, lighting, 1.0f, 1.0f, camera, pass,
                                       presentationAlpha);
                }
            }
            chest.figure.draw(device, clip, lighting, chest.revealed ? 63.0f / 255.0f : 1.0f, 1.0f,
                              camera, TreeModel::Pass::All, presentationAlpha);
        }
    }
}

bool Chests::setLocked(usize index, bool locked) {
    if (index >= m_sim.chests.size()) {
        return false;
    }
    Chest& chest = m_sim.chests[index];
    if (chest.gone || chest.state != kShut) {
        return false;
    }
    chest.locked = locked;
    chest.refusalLeft = 0.0f;
    return true;
}

} // namespace gdl::game
