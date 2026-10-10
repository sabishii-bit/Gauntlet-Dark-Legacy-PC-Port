#include "game/world/Chests.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

#include "game/world/ItemSupport.h"

namespace gdl::game {

namespace {

s16 paramS16(const ItemInstance& instance, usize at) {
    s16 value = 0;
    std::memcpy(&value, &instance.params[at], sizeof(value));
    return value;
}

} // namespace

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

bool Chests::bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items,
                  const WorldCollision* collision, ItemArchive* realmItems) {
    clear();
    m_collision = collision;
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
        auto chest = std::make_unique<Chest>();
        chest->instance = static_cast<s32>(index);
        chest->info = instance.info;
        chest->subtype = info.subtype;
        chest->contents = paramS16(instance, 0);
        chest->count = paramS16(instance, 4);
        chest->minPlayers = instance.minPlayers;
        chest->locked = (static_cast<u32>(info.activeType) & kLocked) != 0;
        const std::string& name = instance.name.empty() ? info.name : instance.name;
        ItemArchive& art = itemArchiveForTree(items, name, realmItems);
        if (!chest->figure.place(device, art, name, instance, collision)) {
            log::warn("Chests: no figure {} in the item archive", name);
        }
        const auto* support =
            itemSupportAt(instance.position, collision, restCollision ? &*restCollision : nullptr);
        if (support != nullptr && (info.collisionFlags & 1U) == 0) {
            constexpr f32 kAbove = 4.0f;
            constexpr f32 kBelow = 10.0f;
            if (const auto floor = support->floorAt(instance.position, kAbove, kBelow, 1)) {
                if (const auto placement = support->objectTransform(floor->object)) {
                    Vec3 position = instance.position;
                    position.y = floor->y + ItemFigure::kFloorLift;
                    chest->floor =
                        Chest::Floor{floor->object, glm::inverse(*placement) *
                                                        itemPlacement(position, instance.rotation)};
                }
            }
        }
        chest->box = chest->figure.obstacle(info);
        m_chests.push_back(std::move(chest));
    }
    syncFloors();
    return !m_chests.empty();
}

void Chests::syncFloors() {
    if (m_collision == nullptr) {
        return;
    }
    for (const auto& entry : m_chests) {
        Chest& chest = *entry;
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
        chest.preview.placeAt(transform);
        chest.box = chest.figure.obstacle(m_infos[static_cast<usize>(chest.info)]);
    }
}

void Chests::clear() {
    m_chests.clear();
    m_infos.clear();
    m_seed = 0;
    m_collision = nullptr;
}

void Chests::setPlayerCount(s32 players) {
    for (const std::unique_ptr<Chest>& chest : m_chests) {
        chest->shown = shownToParty(chest->minPlayers, players);
    }
}

std::vector<ChestEvent> Chests::update(f32 seconds, std::span<const ChestVisitor> party) {
    syncFloors();
    std::vector<ChestEvent> events;
    for (usize index = 0; index < m_chests.size(); ++index) {
        Chest& chest = *m_chests[index];
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
                chest.contents = resolveContents(m_infos, chest.contents,
                                                 static_cast<usize>(chest.instance), m_seed);
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
                if (chest.box.touchedBy(party[visitor].position, party[visitor].radius)) {
                    ChestEvent event;
                    event.kind = ChestEvent::Kind::Collected;
                    event.chest = index;
                    event.visitor = visitor;
                    event.position = chest.figure.position();
                    event.gold = m_infos[static_cast<usize>(chest.contents)].value;
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
    if (index >= m_chests.size()) {
        return false;
    }
    Chest& chest = *m_chests[index];
    if (!chest.shown || chest.gone || chest.state != kShut) {
        return false;
    }
    chest.contents = record;
    chest.wobble = std::trunc(kWobblePerPower * power);
    for (const auto& other : m_chests) {
        if (other->info == chest.info) {
            other->subtype = kTransmuted;
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
                            const WorldLayout& layout, ItemArchive& items,
                            const WorldCollision* collision, std::string_view figure) {
    if (index >= m_chests.size()) {
        return false;
    }
    Chest& chest = *m_chests[index];
    if (!chest.shown || chest.gone || chest.state != kShut) {
        return false;
    }
    chest.contents = record;
    if (!figure.empty() && items.trees.find(figure).has_value()) {
        const Mat4 placement = chest.figure.transform();
        const ItemInstance& instance = layout.itemInstances()[static_cast<usize>(chest.instance)];
        chest.figure.place(device, items, figure, instance, collision);
        chest.figure.placeAt(placement);
        chest.figure.play(kShut, true);
    }
    return true;
}

void Chests::hold(usize chest, s32 item) {
    if (chest < m_chests.size()) {
        m_chests[chest]->held = item;
    }
}

usize Chests::updateXray(RenderDevice& device, ItemArchive& items, ItemArchive& powerups,
                         f32 seconds, std::span<const ChestVisitor> party,
                         ItemArchive* realmItems) {
    std::vector<bool> selected(m_chests.size(), false);
    for (const auto& visitor : party) {
        if (!visitor.xray) {
            continue;
        }
        f32 best = 10.0f * 10.0f;
        usize closest = m_chests.size();
        for (usize i = 0; i < m_chests.size(); ++i) {
            const auto& chest = *m_chests[i];
            // ClosestChest checks visibility and action, not the container subtype:
            // X-Ray also warns about a bomb inside a trapped chest.
            if (!chest.shown || chest.gone || chest.state != kShut) {
                continue;
            }
            u32 seed = m_seed;
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
    for (usize i = 0; i < m_chests.size(); ++i) {
        auto& chest = *m_chests[i];
        if (selected[i]) {
            fresh += chest.revealed ? 0U : 1U;
            u32 seed = m_seed; // Looking must not change what opening a random chest produces.
            const s32 inside =
                resolveContents(m_infos, chest.contents, static_cast<usize>(chest.instance), seed);
            if (inside != chest.previewContents) {
                chest.previewContents = inside;
                const auto& record = m_infos[static_cast<usize>(inside)];
                std::string_view name = record.name;
                if (record.type == ItemInfo::kPlacedEnemy) {
                    name = "DEATH_ICON";
                } else if (record.subtype == 2 && chest.count > 1) {
                    name = "KEYRING";
                }
                ItemArchive& levelArt = itemArchiveForTree(items, name, realmItems);
                ItemArchive& archive = itemArchiveForTree(levelArt, name, &powerups);
                ItemInstance instance;
                instance.position = chest.figure.position();
                instance.rotation.y = -chest.figure.yaw();
                chest.preview.place(device, archive, name, instance, nullptr);
            }
            chest.preview.update(seconds);
        }
        chest.revealed = selected[i];
    }
    return fresh;
}

s32 Chests::holdingTouchedBy(const ChestVisitor& visitor) const {
    for (usize index = 0; index < m_chests.size(); ++index) {
        const Chest& chest = *m_chests[index];
        if (chest.shown && !chest.gone && chest.state == kOpen && chest.held >= 0 &&
            chest.box.touchedBy(visitor.position, visitor.radius)) {
            return static_cast<s32>(index);
        }
    }
    return -1;
}

void Chests::remove(usize chest) {
    if (chest < m_chests.size()) {
        m_chests[chest]->gone = true;
        m_chests[chest]->held = -1;
    }
}

std::vector<Obstacle> Chests::obstacles() const {
    std::vector<Obstacle> boxes;
    for (const std::unique_ptr<Chest>& chest : m_chests) {
        // ItemTouch (8005D730) leaves an open pickup chest passable even when a
        // full-health player cannot take its food. Gold uses its own touch path.
        const bool holdsPickup =
            chest->state == kOpen && chest->held >= 0 && chest->subtype != kGoldChest;
        if (chest->shown && !chest->gone && !holdsPickup) {
            boxes.push_back(chest->box);
        }
    }
    return boxes;
}

void Chests::capturePresentation() {
    for (const auto& chest : m_chests) {
        chest->figure.capturePresentation();
        chest->preview.capturePresentation();
    }
}

void Chests::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                  const CameraFrame* camera, f32 presentationAlpha) const {
    for (const std::unique_ptr<Chest>& chest : m_chests) {
        if (chest->shown && !chest->gone) {
            if (chest->revealed) {
                // Retail MBTreeSetAlpha(0xC0) is transparency, not opacity.
                chest->preview.draw(device, clip, lighting, 1.0f, 0.65f, camera,
                                    TreeModel::Pass::All, presentationAlpha);
            }
            chest->figure.draw(device, clip, lighting, chest->revealed ? 63.0f / 255.0f : 1.0f,
                               1.0f, camera, TreeModel::Pass::All, presentationAlpha);
        }
    }
}

} // namespace gdl::game
