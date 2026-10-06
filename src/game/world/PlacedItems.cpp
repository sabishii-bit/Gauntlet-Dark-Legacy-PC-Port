#include "game/world/PlacedItems.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

#include "game/players/ItemPickup.h"
#include "game/world/ItemFigure.h"

namespace gdl::game {
namespace {
// External relocations larger than a normal pickup/platform tick are visual cuts.
constexpr f32 kPresentationCutDistance = 32.0f;

Mat4 blendPlacement(const Mat4& previous, const Mat4& current, f32 blend) {
    Mat3 from{previous};
    Mat3 to{current};
    Vec3 fromScale{0};
    Vec3 toScale{0};
    for (s32 axis = 0; axis < 3; ++axis) {
        fromScale[axis] = glm::length(from[axis]);
        toScale[axis] = glm::length(to[axis]);
        if (fromScale[axis] < 1e-6f || toScale[axis] < 1e-6f) {
            return current;
        }
        from[axis] /= fromScale[axis];
        to[axis] /= toScale[axis];
    }
    const auto orthogonal = [](const Mat3& basis) {
        return std::abs(glm::determinant(basis) - 1.0f) < 1e-3f &&
               std::abs(glm::dot(basis[0], basis[1])) < 1e-3f &&
               std::abs(glm::dot(basis[0], basis[2])) < 1e-3f &&
               std::abs(glm::dot(basis[1], basis[2])) < 1e-3f;
    };
    // A sheared or reflected socket keeps its authored basis, not an unrelated rotation.
    Mat4 result = current;
    if (orthogonal(from) && orthogonal(to)) {
        result = glm::mat4_cast(glm::slerp(glm::quat_cast(from), glm::quat_cast(to), blend));
        result = glm::scale(result, glm::mix(fromScale, toScale, blend));
    }
    result[3] = glm::mix(previous[3], current[3], blend);
    return result;
}
} // namespace

void PlacedItems::attach(usize index, const Mat4& transform, bool contained) {
    if (index < m_items.size()) {
        Item& item = m_items[index];
        if (item.contained != contained) {
            item.presentationCaptured = false;
        }
        item.floor.reset();
        item.transform = transform;
        item.position = Vec3{transform[3]};
        item.contained = contained;
    }
}

void PlacedItems::discard(usize index) {
    if (index < m_items.size()) {
        m_items[index].taken = true;
        m_items[index].visible = false;
    }
}

bool PlacedItems::Item::shownTo(s32 players) const {
    if (minPlayers > kExactPlayersMark) {
        return players == minPlayers - kExactPlayersMark;
    }
    return players >= minPlayers;
}

/** Within the two radii sideways and the item's height plus the collector's slack up or
 * down. */
bool PlacedItems::Item::touchedBy(const Collector& collector) const {
    const Vec3 away = collector.position - position;
    const f32 reach = radius + collector.radius;
    if (away.x * away.x + away.z * away.z > reach * reach) {
        return false;
    }
    return std::abs(away.y) <= height + collector.height;
}

s32 PlacedItems::Item::realm() const {
    if (subtype != ItemInfo::kCrystal || value < 0 ||
        static_cast<usize>(value) >= kCrystalRealms.size()) {
        return -1;
    }
    return kCrystalRealms[static_cast<usize>(value)];
}

bool PlacedItems::bind(RenderDevice& device, const WorldLayout& layout,
                       const WorldCollision* collision, std::span<ItemArchive* const> archives) {
    clear();
    m_collision = collision;
    // Authored pickup positions describe the layout's rest pose. Find their
    // supporting objects there, then let the current animation pose carry them.
    std::optional<WorldCollision> restCollision;
    if (collision != nullptr && collision->movingObjectCount() != 0) {
        restCollision = *collision;
        for (usize i = 0; i < layout.objects().size(); ++i) {
            restCollision->setObjectTransform(static_cast<s32>(i),
                                              glm::translate(Mat4{1.0f}, layout.worldPosition(i)));
        }
    }
    m_archives.assign(archives.begin(), archives.end());
    m_infos.clear();
    for (ItemArchive* archive : archives) {
        if (archive == nullptr || !archive->loaded()) {
            continue;
        }
        ArchiveMotion motion;
        motion.archive = archive;
        motion.animator.bind(archive->trees.textureAnimations(), archive->textures, device);
        m_motions.push_back(std::move(motion));
    }
    const std::vector<ItemInfo>& infos = layout.itemInfos();
    m_infos = infos;
    const std::vector<ItemInstance>& instances = layout.itemInstances();
    for (usize index = 0; index < instances.size(); ++index) {
        const ItemInstance& instance = instances[index];
        if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size()) {
            continue;
        }
        const ItemInfo& info = infos[static_cast<usize>(instance.info)];
        if (info.type != ItemInfo::kPowerup) {
            continue;
        }
        Item item;
        item.name = instance.name.empty() ? info.name : instance.name;
        item.instance = static_cast<s32>(index);
        item.info = instance.info;
        item.subtype = info.subtype;
        item.value = info.value;
        item.health = info.hitPoints;
        if (info.subtype == ItemInfo::kScroll) {
            // A scroll's page is the instance's, from one.
            item.value = static_cast<s16>(instance.params[0] | (instance.params[1] << 8));
        }
        item.flags = info.properties;
        item.strength = static_cast<f32>(info.activeOn);
        item.minPlayers = instance.minPlayers;
        item.radius = info.radius;
        item.height = info.height;
        if (!makeFigure(device, item)) {
            log::warn("Placed items: no archive holds {}", item.name);
            continue;
        }
        item.position = instance.position;
        item.transform = itemPlacement(item.position, instance.rotation);
        restOnFloor(item, restCollision ? &*restCollision : collision, kFloorLift);
        item.visible = item.shownTo(m_players);
        m_items.push_back(std::move(item));
    }
    syncFloors();
    return !m_items.empty();
}

void PlacedItems::restOnFloor(Item& item, const WorldCollision* collision, f32 lift) {
    item.floor.reset();
    if (collision == nullptr) {
        return;
    }
    const auto floor = collision->floorAt(item.position, kFloorReachAbove, kFloorReachBelow);
    if (!floor) {
        return;
    }
    item.position.y = floor->y + lift;
    item.transform[3] = Vec4{item.position, 1.0f};
    if (const auto placement = collision->objectTransform(floor->object)) {
        item.floor = Item::Floor{floor->object, glm::inverse(*placement) * item.transform};
    }
}

void PlacedItems::syncFloors() {
    if (m_collision == nullptr) {
        return;
    }
    for (Item& item : m_items) {
        if (!item.floor || item.taken || item.carried || item.contained || item.thrown) {
            continue;
        }
        const auto placement = m_collision->objectTransform(item.floor->object);
        // AddItemSub's scene-graph parent survives temporary collision disabling
        // during platform animations (notably Maze of Illusion).
        if (!placement) {
            item.floor.reset();
            continue;
        }
        item.transform = *placement * item.floor->local;
        item.position = Vec3{item.transform[3]};
    }
}

void PlacedItems::clear() {
    m_infos.clear();
    m_items.clear();
    m_effects.clear();
    m_bursts.clear();
    m_archives.clear();
    m_motions.clear();
    m_collision = nullptr;
    m_revealTime = 0.0f;
    m_revealing = false;
    m_capturePending = false;
    m_burstsAdvanced = false;
}

usize PlacedItems::visibleCount() const {
    usize count = 0;
    for (const Item& item : m_items) {
        count += item.visible ? 1 : 0;
    }
    return count;
}

void PlacedItems::setPlayerCount(s32 players) {
    m_players = players;
    for (Item& item : m_items) {
        const bool wasVisible = item.visible;
        item.visible = !item.taken && !item.carried && item.shownTo(players);
        if (item.visible != wasVisible) {
            item.presentationCaptured = false;
        }
    }
}

void PlacedItems::retireCrystals() {
    for (Item& item : m_items) {
        if (item.subtype == ItemInfo::kCrystal) {
            item.taken = true;
            item.visible = false;
        }
    }
}

usize PlacedItems::poisonFood(RenderDevice& device, const Vec3& position, f32 radius, f32 damage) {
    // items.c::fn_8005C1DC (0x8005C1DC), DMG_POISONGAS: power above 2
    // replaces food's figure and value. The record's hitpoints distinguish meat
    // from fruit, not its name or healing amount. All shipped food uses cylinders.
    if (damage <= 2.0f || radius <= 0.0f) {
        return 0;
    }
    usize changed = 0;
    for (Item& item : m_items) {
        if (item.subtype != static_cast<s32>(ItemKind::Food) ||
            !exposedWithin(item, position, radius)) {
            continue;
        }
        const ItemInfo& info = m_infos[static_cast<usize>(item.info)];
        if (info.armor == -1) {
            continue;
        }
        const bool meat = info.hitPoints == 2;
        const std::string_view name = meat ? "BADMEAT" : "GAPPLE";
        const s32 value = meat ? -100 : -50;
        if (item.name == name && item.value == value) {
            continue;
        }
        if (!replaceFigure(device, item, name)) {
            continue;
        }
        item.value = value;
        ++changed;
    }
    return changed;
}

std::vector<PlacedItems::PerkChange> PlacedItems::bless(RenderDevice& device, const Vec3& position,
                                                        f32 radius, MagicPerk perk) {
    constexpr s32 kMostJunk = 10;
    constexpr s32 kRottenMeat = -100; ///< this or less is meat gone bad
    constexpr s32 kSilver = 100;
    constexpr s32 kGold = 200;
    constexpr s32 kApple = 50;
    constexpr s32 kChicken = 100;
    struct Turn {
        std::string_view figure;
        s32 value = 0;
        MagicPerkDeed deed = MagicPerkDeed::JunkToSilver;
    };
    std::vector<PerkChange> changes;
    for (Item& item : m_items) {
        if (!exposedWithin(item, position, radius)) {
            continue;
        }
        std::optional<Turn> turn;
        const auto kind = static_cast<ItemKind>(item.subtype);
        if (perk.family == MagicPerkFamily::Treasure && kind == ItemKind::Gold &&
            item.value <= kMostJunk) {
            turn = perk.greater ? Turn{"TREAS_GOLD", kGold, MagicPerkDeed::JunkToGold}
                                : Turn{"TREAS_SILVER", kSilver, MagicPerkDeed::JunkToSilver};
        } else if (perk.family == MagicPerkFamily::Food && kind == ItemKind::Food &&
                   item.value < 0) {
            if (item.value > kRottenMeat) {
                turn = Turn{"APPLE", kApple, MagicPerkDeed::CleanseFruit};
            } else if (perk.greater) {
                turn = Turn{"CHICKEN", kChicken, MagicPerkDeed::CleanseMeat};
            }
        }
        if (turn.has_value() && replaceFigure(device, item, turn->figure)) {
            item.value = turn->value;
            changes.push_back({turn->deed, item.transform});
        }
    }
    return changes;
}

bool PlacedItems::exposedWithin(const Item& item, const Vec3& position, f32 radius) const {
    if (!item.visible || item.taken || item.contained || item.info < 0 ||
        static_cast<usize>(item.info) >= m_infos.size() || radius <= 0) {
        return false;
    }
    const ItemInfo& info = m_infos[static_cast<usize>(item.info)];
    const Vec3 centre = Vec3{item.transform * Vec4{info.collisionOffset, 1}};
    const Vec3 delta = centre - position;
    return info.collisionType == 1 && std::hypot(delta.x, delta.z) <= radius + item.radius &&
           std::abs(delta.y) <= radius + item.height;
}

bool PlacedItems::replaceFigure(RenderDevice& device, Item& item, std::string_view name) {
    // A failed asset load leaves both the current figure and pickup value intact.
    Item replacement;
    replacement.name = name;
    if (!makeFigure(device, replacement)) {
        return false;
    }
    item.name = std::move(replacement.name);
    item.model = std::move(replacement.model);
    item.particles = std::move(replacement.particles);
    item.pose = std::move(replacement.pose);
    item.figure = replacement.figure;
    item.archive = replacement.archive;
    item.player = replacement.player;
    item.presentationCaptured = false;
    return true;
}

/** Item damage (0x8005C1DC) on a bottle: its health less its armour, a hit always counting
 * for one, not the five-point threshold food and treasure need; broken, it is gone and its
 * kind comes back. */
std::optional<s32> PlacedItems::damagePotion(Item& item, const ItemInfo& info, f32 damage) {
    if (info.armor < 0) {
        return std::nullopt;
    }
    const f32 afterArmor = damage - static_cast<f32>(info.armor);
    const f32 power = afterArmor <= 0 ? 1.0f : afterArmor;
    item.health = std::max(0, item.health - static_cast<s32>(std::round(power)));
    if (item.health != 0) {
        return std::nullopt;
    }
    item.taken = true;
    item.visible = false;
    return static_cast<s32>(item.flags & kPotionKind);
}

std::vector<usize> PlacedItems::shootablePotions() const {
    std::vector<usize> found;
    for (usize index = 0; index < m_items.size(); ++index) {
        const Item& item = m_items[index];
        if (static_cast<ItemKind>(item.subtype) == ItemKind::Potion && item.takeable() &&
            !item.thrown && item.info >= 0 && m_infos[static_cast<usize>(item.info)].armor >= 0) {
            found.push_back(index);
        }
    }
    return found;
}

std::optional<s32> PlacedItems::strikePotion(usize index, f32 damage) {
    if (index >= m_items.size()) {
        return std::nullopt;
    }
    Item& item = m_items[index];
    if (static_cast<ItemKind>(item.subtype) != ItemKind::Potion || !item.takeable() ||
        item.info < 0) {
        return std::nullopt;
    }
    return damagePotion(item, m_infos[static_cast<usize>(item.info)], damage);
}

std::vector<PlacedItems::BlastChange> PlacedItems::blast(RenderDevice& device, const Vec3& position,
                                                         f32 radius, f32 damage,
                                                         bool destroysPickups) {
    constexpr f32 kDestroyPower = 5;
    constexpr s32 kJunkValue = 10;
    std::vector<BlastChange> changes;
    if (damage <= 0) {
        return changes;
    }
    for (Item& item : m_items) {
        if (!exposedWithin(item, position, radius)) {
            continue;
        }
        const auto kind = static_cast<ItemKind>(item.subtype);
        const ItemInfo& info = m_infos[static_cast<usize>(item.info)];
        if (kind == ItemKind::Potion) {
            // Held chest contents never reach this branch until released.
            if (const auto broken = damagePotion(item, info, damage); broken.has_value()) {
                changes.push_back({item.position, true, broken, item.transform});
            }
            continue;
        }
        const bool treasure = kind == ItemKind::Gold;
        if (!destroysPickups) {
            continue; // fn_8005C1DC requires DMG_EXPLODE, not merely fire, for this destruction.
        }
        const bool destructible =
            kind == ItemKind::Food || kind == ItemKind::WeaponPowerup ||
            kind == ItemKind::ArmorPowerup || kind == ItemKind::SpeedPowerup ||
            kind == ItemKind::MagicPowerup || kind == ItemKind::SpecialPowerup;
        if (!treasure && !destructible) {
            continue;
        }
        const f32 power =
            info.armor >= 0 ? std::max(1.0f, damage - static_cast<f32>(info.armor)) : damage;
        if (power < kDestroyPower) {
            continue;
        }
        if (treasure) {
            if ((item.name == "TREAS_JUNK" && item.value == kJunkValue) ||
                !replaceFigure(device, item, "TREAS_JUNK")) {
                continue;
            }
            item.value = kJunkValue;
        } else {
            item.taken = true;
            item.visible = false;
        }
        changes.push_back({item.position, !treasure, std::nullopt, item.transform});
    }
    return changes;
}

bool PlacedItems::makeFigure(RenderDevice& device, Item& item) {
    for (ItemArchive* archive : m_archives) {
        if (archive == nullptr || !archive->loaded()) {
            continue;
        }
        const auto tree = archive->trees.find(item.name);
        if (!tree.has_value()) {
            continue;
        }
        const TreeInfo& figure = archive->trees.tree(*tree);
        if (item.model.bind(figure, archive->models, archive->textures, device)) {
            item.figure = &figure;
            item.archive = archive;
            if (figure.sequences.empty()) {
                item.pose.rest(figure);
            } else {
                item.player.start(figure.sequences[0], 0);
                item.pose.evaluate(figure, 0, 0.0f);
                item.model.setFrame(0, 0);
            }
            // AtreeNodeInit also instantiates type-4 children. QUEST_PARCH's
            // POOLFIRE is part of the pickup tree, not its collection effect.
            item.particles.bind(figure, *archive, device, item.transform, item.pose.matrices());
            item.particles.setLocalScales(item.pose.poses());
            return true;
        }
    }
    return false;
}

bool PlacedItems::place(RenderDevice& device, std::string_view name, const Vec3& position,
                        const WorldCollision* collision) {
    const auto info = std::ranges::find_if(m_infos, [&](const ItemInfo& candidate) {
        return candidate.type == ItemInfo::kPowerup && candidate.name == name;
    });
    if (info == m_infos.end()) {
        log::warn("Placed items: the level has no item record named {}", name);
        return false;
    }
    return placeRecord(device, static_cast<s32>(info - m_infos.begin()), position, collision);
}

bool PlacedItems::placeRecord(RenderDevice& device, s32 record, const Vec3& position,
                              const WorldCollision* collision, s32 amount) {
    if (record < 0 || static_cast<usize>(record) >= m_infos.size() ||
        m_infos[static_cast<usize>(record)].type != ItemInfo::kPowerup) {
        return false;
    }
    const ItemInfo* info = &m_infos[static_cast<usize>(record)];
    Item item;
    item.name = info->name;
    item.info = record;
    item.subtype = info->subtype;
    item.value = amount > 0 ? amount : info->value;
    item.health = info->hitPoints;
    item.flags = info->properties;
    item.strength = static_cast<f32>(info->activeOn);
    item.radius = info->radius;
    item.height = info->height;
    if (!makeFigure(device, item)) {
        log::warn("Placed items: no archive holds {}", item.name);
        return false;
    }
    item.position = position;
    item.transform = itemPlacement(item.position, Vec3{0.0f, 0.0f, 0.0f});
    restOnFloor(item, collision, kFloorLift);
    if (collision != nullptr) {
        m_collision = collision;
    }
    item.visible = true;
    m_items.push_back(std::move(item));
    return true;
}

/** Placed where it starts (not on the floor) and set flying; with no floor to land on it
 * stays where it is thrown. */
bool PlacedItems::throwItem(RenderDevice& device, std::string_view name, const Vec3& position,
                            const Vec3& velocity, const WorldCollision* collision,
                            f32 noGrabSeconds, std::optional<f32> strength) {
    if (!place(device, name, position, collision)) {
        return false;
    }
    Item& item = m_items.back();
    item.floor.reset();
    item.noGrabSeconds = noGrabSeconds;
    if (strength.has_value()) {
        item.strength = *strength;
    }
    if (collision != nullptr) {
        item.position = position;
        item.transform = itemPlacement(item.position, Vec3{0.0f, 0.0f, 0.0f});
        item.velocity = velocity;
        item.thrown = true;
        m_collision = collision;
    }
    return true;
}

std::optional<usize> PlacedItems::claim(const Vec3& position, f32 reach, f32 rise) {
    std::optional<usize> nearest;
    f32 best = reach;
    for (usize i = 0; i < m_items.size(); ++i) {
        const Item& item = m_items[i];
        if (item.taken || item.carried || item.contained || item.thrown || item.minPlayers > 1 ||
            std::abs(item.position.y - position.y) >= rise) {
            continue;
        }
        const f32 along = std::hypot(item.position.x - position.x, item.position.z - position.z);
        if (along < best) {
            best = along;
            nearest = i;
        }
    }
    if (nearest.has_value()) {
        m_items[*nearest].floor.reset();
        m_items[*nearest].carried = true;
        m_items[*nearest].visible = false;
    }
    return nearest;
}

bool PlacedItems::release(usize index, const Vec3& position, const Vec3& velocity,
                          const WorldCollision* collision, f32 noGrabSeconds) {
    if (index >= m_items.size() || !m_items[index].carried) {
        return false;
    }
    Item& item = m_items[index];
    item.carried = false;
    item.presentationCaptured = false;
    item.floor.reset();
    item.visible = !item.taken && item.shownTo(m_players);
    item.position = position;
    item.transform = itemPlacement(item.position, Vec3{0.0f, 0.0f, 0.0f});
    item.noGrabSeconds = noGrabSeconds;
    if (collision != nullptr) {
        item.velocity = velocity;
        item.thrown = true;
        m_collision = collision;
    }
    return true;
}

bool PlacedItems::goldLeft() const {
    return std::ranges::any_of(m_items, [](const Item& item) {
        return item.visible && !item.taken && item.subtype == ItemInfo::kGold;
    });
}

std::vector<Pickup> PlacedItems::collect(RenderDevice& device,
                                         std::span<const Collector> collectors,
                                         const PickupJudge& judge) {
    std::vector<Pickup> pickups;
    for (usize i = 0; i < m_items.size(); ++i) {
        Item& item = m_items[i];
        if (!item.takeable()) {
            continue;
        }
        usize taker = collectors.size();
        for (usize c = 0; c < collectors.size() && taker == collectors.size(); ++c) {
            if (item.touchedBy(collectors[c])) {
                taker = c;
            }
        }
        if (taker == collectors.size()) {
            continue;
        }
        Pickup pickup;
        pickup.item = i;
        pickup.collector = taker;
        pickup.subtype = item.subtype;
        pickup.realm = item.realm();
        pickup.amount = item.value;
        pickup.flags = item.flags;
        pickup.strength = item.strength;
        pickup.position = item.position;
        pickup.opener = item.opener;
        if (judge) {
            const std::optional<s32> left = judge(pickup);
            if (!left.has_value()) {
                continue; // left lying
            }
            if (*left > 0) {
                item.value = *left; // the rest stays for the next to come by
                pickups.push_back(pickup);
                continue;
            }
        }
        item.taken = true;
        item.visible = false;
        if (pickup.realm > 0 && static_cast<usize>(pickup.realm) < kGemEffects.size()) {
            startEffect(device, kGemEffects[static_cast<usize>(pickup.realm)], item.position);
        } else if (item.subtype == ItemInfo::kRunestone) {
            startEffect(device, kRuneEffect, item.position);
        } else if (item.subtype == ItemInfo::kGargoyleKey) {
            startEffect(device, kGargoyleEffect, item.position);
        }
        pickups.push_back(pickup);
    }
    return pickups;
}

/** Starts a tree's particle nodes at `position`, riding its first sequence, from whichever
 * archive holds the tree. */
void PlacedItems::startEffect(RenderDevice& device, std::string_view tree, const Vec3& position) {
    for (ItemArchive* archive : m_archives) {
        if (archive == nullptr || !archive->loaded()) {
            continue;
        }
        const auto index = archive->trees.find(tree);
        if (!index.has_value()) {
            continue;
        }
        const TreeInfo& figure = archive->trees.tree(*index);
        const std::vector<ParticleTemplate>& templates = archive->trees.particleTemplates();
        Effect effect;
        effect.figure = &figure;
        effect.position = position;
        if (figure.sequences.empty()) {
            effect.pose.rest(figure);
            effect.secondsLeft = kBurstFallbackSeconds;
        } else {
            effect.player.start(figure.sequences[0], 0);
            effect.pose.evaluate(figure, 0, 0.0f);
        }
        const Mat4 base = glm::translate(Mat4{1.0f}, position);
        for (usize n = 0; n < figure.nodes.size(); ++n) {
            const TreeNodeInfo& node = figure.nodes[n];
            if (node.particle < 0 || static_cast<usize>(node.particle) >= templates.size()) {
                continue;
            }
            ParticleDescriptor descriptor =
                ParticleDescriptor::fromTemplate(templates[static_cast<usize>(node.particle)]);
            // A node's own direction stands in when the template names none.
            if (!templates[static_cast<usize>(node.particle)].sets(ParticleTemplate::kDirection) &&
                node.direction != Vec3{0.0f, 0.0f, 0.0f}) {
                descriptor.direction = node.direction;
            }
            const Texture* texture = nullptr;
            if (const auto found = archive->textures.find(descriptor.texture); found.has_value()) {
                try {
                    texture = &archive->textures.texture(device, *found);
                } catch (const std::exception& e) {
                    log::warn("Placed items: burst texture {}: {}", descriptor.texture, e.what());
                }
            }
            const Mat4 at = n < effect.pose.matrices().size()
                                ? base * effect.pose.matrices()[n]
                                : glm::translate(base, figure.worldPosition(n));
            effect.emitters.emplace_back(
                n, m_bursts.start(descriptor, at,
                                  texture != nullptr ? texture : &device.whiteTexture(),
                                  static_cast<u32>(m_effects.size() * 8 + n + 1)));
        }
        if (!effect.emitters.empty()) {
            m_effects.push_back(std::move(effect));
        }
        return;
    }
    log::warn("Placed items: no archive holds the burst {}", tree);
}

/** Shows every item the frames its archive's animations have reached and the way their
 * scrolls have slid, scrolls on one texture adding up. */
void PlacedItems::applyTextureMotion() {
    for (Item& item : m_items) {
        if (!item.visible || item.archive == nullptr) {
            continue;
        }
        for (const ArchiveMotion& motion : m_motions) {
            if (motion.archive != item.archive) {
                continue;
            }
            if (item.figure != nullptr) {
                motion.animator.apply(item.model, *item.figure, item.player.sequence(),
                                      static_cast<s32>(item.player.frame()));
                motion.animator.apply(item.particles, *item.figure, item.player.sequence(),
                                      static_cast<s32>(item.player.frame()));
            }
        }
    }
}

/** A thrown item's flight, as the original's coins fly: it falls under gravity, and on
 * touching the floor (where a placed item rests) bounces back at a share of its speed
 * until a bounce would not clear the touching-down height, when it stays down; sideways it
 * slows a little in the air and much more on touching down, and stops once it has all but
 * stopped. */
void PlacedItems::fly(Item& item, f32 seconds) {
    const f32 previousY = item.position.y;
    item.position += item.velocity * seconds;
    f32 over = kThrownFloorReach;
    f32 drag = kAirDrag * seconds;
    if (item.velocity.y <= 0.0f && m_collision != nullptr) {
        // Include the downward step: a fast coin can cross the floor between updates.
        const f32 above = std::max(kFloorReachAbove, previousY - item.position.y);
        const auto floor = m_collision->floorAt(item.position, above, kThrownFloorReach);
        if (!floor && item.position.y < m_collision->lowest() - kThrownFloorReach) {
            // A missing floor while airborne can just be a crack. Retire it only after
            // falling below the level, not while its horizontal flight can reach land.
            item.visible = false;
            item.taken = true;
            item.thrown = false;
            return;
        }
        if (floor) {
            const f32 rest = floor->y + kThrownFloorLift;
            over = item.position.y - rest;
            if (over < kRestHeight) {
                item.velocity.y = -kBounce * item.velocity.y;
                if (item.velocity.y * item.velocity.y < 2.0f * kGravity * kRestHeight) {
                    item.velocity.y = 0.0f;
                }
                item.position.y = rest;
                drag = kGroundDrag * seconds;
            }
        }
    }
    if (over >= kRestHeight) {
        item.velocity.y -= kGravity * seconds;
    }
    const auto slow = [drag](f32& v) { v = std::abs(v) > drag ? v - drag * v : 0.0f; };
    slow(item.velocity.x);
    slow(item.velocity.z);
    item.transform = itemPlacement(item.position, Vec3{0.0f, 0.0f, 0.0f});
    if (item.velocity == Vec3{0.0f, 0.0f, 0.0f}) {
        item.thrown = false;
        restOnFloor(item, m_collision, kThrownFloorLift);
    }
}

void PlacedItems::capturePresentation() {
    for (auto& motion : m_motions) {
        motion.animator.advance(0);
    }
    m_capturePending = true;
    m_burstsAdvanced = false;
    for (Item& item : m_items) {
        item.previousTransform = item.transform;
        item.previousFrame = item.player.presentationFrame();
        item.previousGeneration = item.player.generation();
        item.previousAlpha = item.alpha;
        item.presentationCaptured = item.visible;
    }
}

void PlacedItems::snapPresentation() {
    for (Item& item : m_items) {
        item.presentationCaptured = false;
    }
}

void PlacedItems::update(f32 seconds) {
    if (!m_capturePending) {
        capturePresentation();
    }
    m_capturePending = false;
    m_burstsAdvanced = seconds > 0;
    syncFloors();
    // The archives' texture animations step once a game frame: the sheen on the crystals.
    for (ArchiveMotion& motion : m_motions) {
        motion.animator.advance(seconds);
    }
    // The figures on show play their sequence over and over.
    for (Item& item : m_items) {
        if (item.visible && item.figure != nullptr && item.player.playing()) {
            item.player.advance(seconds, true);
            item.pose.evaluate(*item.figure, item.player.sequence(), item.player.frame());
            item.model.setFrame(item.player.sequence(), static_cast<s32>(item.player.frame()));
        }
        item.noGrabSeconds = std::max(item.noGrabSeconds - seconds, 0.0f);
        if (item.thrown) {
            fly(item, seconds);
        }
        item.particles.setEmitting(item.visible);
        item.particles.setLocalScales(item.pose.poses());
        item.particles.step(seconds, item.transform, item.pose.matrices());
    }
    // Sequence-keyed textures (including boss coins) follow the new pose frame,
    // even on a render frame without a whole free-running texture tick.
    applyTextureMotion();
    // A burst's emitters ride their nodes through the tree's sequence, then stop emitting.
    for (Effect& effect : m_effects) {
        if (!effect.emitting) {
            continue;
        }
        bool over = false;
        if (effect.player.playing()) {
            effect.player.advance(seconds, false);
            effect.pose.evaluate(*effect.figure, effect.player.sequence(), effect.player.frame());
            const Mat4 base = glm::translate(Mat4{1.0f}, effect.position);
            for (const auto& [node, emitter] : effect.emitters) {
                if (node < effect.pose.matrices().size()) {
                    m_bursts.setNode(emitter, base * effect.pose.matrices()[node]);
                }
            }
            over = effect.player.finished();
        } else {
            effect.secondsLeft -= seconds;
            over = effect.secondsLeft <= 0.0f;
        }
        if (over) {
            effect.emitting = false;
            for (const auto& [node, emitter] : effect.emitters) {
                m_bursts.stop(emitter);
            }
        }
    }
    m_bursts.step(seconds);
    // A burst is over once nothing of it is left to show; then the field forgets them all.
    std::erase_if(m_effects, [&](const Effect& effect) {
        return std::ranges::none_of(effect.emitters, [&](const std::pair<usize, usize>& ride) {
            return m_bursts.active(ride.second);
        });
    });
    if (m_effects.empty()) {
        m_bursts.prune();
    }
}

void PlacedItems::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                       const CameraFrame* camera, TreeModel::Pass pass, f32 frameBlend) const {
    const f32 blend = std::clamp(frameBlend, 0.0f, 1.0f);
    for (const Item& item : m_items) {
        if (item.visible) {
            const TreePose* pose = &item.pose;
            Mat4 transform = item.transform;
            f32 alpha = item.alpha;
            f32 visualFrame = item.player.frame();
            if (frameBlend >= 0 && item.presentationCaptured &&
                glm::distance(Vec3{item.previousTransform[3]}, item.position) <=
                    kPresentationCutDistance) {
                transform = blendPlacement(item.previousTransform, item.transform, blend);
                alpha = glm::mix(item.previousAlpha, item.alpha, blend);
                if (item.figure != nullptr && item.player.playing()) {
                    visualFrame =
                        item.previousGeneration == item.player.generation()
                            ? glm::mix(item.previousFrame, item.player.presentationFrame(), blend)
                            : item.player.presentationFrame();
                    item.presentationPose.evaluate(*item.figure, item.player.sequence(),
                                                   visualFrame, false, true);
                    pose = &item.presentationPose;
                }
            }
            if (item.figure != nullptr) {
                for (const auto& motion : m_motions) {
                    if (motion.archive == item.archive) {
                        motion.animator.apply(item.model, *item.figure, item.player.sequence(),
                                              visualFrame,
                                              motion.animator.presentationOffset(frameBlend));
                        motion.animator.apply(item.particles, *item.figure, item.player.sequence(),
                                              visualFrame,
                                              motion.animator.presentationOffset(frameBlend));
                    }
                }
                item.model.setPresentationFrame(item.player.sequence(), visualFrame);
            }
            item.model.draw(device, clip, transform, lighting, pose->matrices(), camera, alpha,
                            pass);
            if (pass != TreeModel::Pass::DepthWriting) {
                const CameraFrame frame = camera != nullptr ? *camera : CameraFrame{};
                item.particles.draw(device, clip, frame.right, frame.up,
                                    m_burstsAdvanced ? frameBlend : -1.0f);
            }
        }
    }
    if (pass != TreeModel::Pass::DepthWriting) {
        const CameraFrame frame = camera != nullptr ? *camera : CameraFrame{};
        m_bursts.draw(device, clip, frame.right, frame.up, m_burstsAdvanced ? frameBlend : -1.0f);
    }
}

void PlacedItems::hideCrystals() {
    m_revealTime = 0.0f;
    m_revealing = false;
    for (Item& item : m_items) {
        if (item.subtype == ItemInfo::kCrystal) {
            item.alpha = 0.0f;
            item.presentationCaptured = false;
            m_revealing = true;
        }
    }
}

/** The reveal spreads from the world's origin at its pace; a crystal it has reached gains a
 * step of alpha a frame until it is whole. */
void PlacedItems::reveal(f32 seconds) {
    if (!m_revealing) {
        return;
    }
    m_revealTime += seconds;
    const f32 reach = kRevealSpread * (kRevealLead + m_revealTime);
    const f32 gain = seconds * kFrameRate * kRevealStep;
    bool left = false;
    for (Item& item : m_items) {
        if (item.subtype != ItemInfo::kCrystal || item.alpha >= 1.0f) {
            continue;
        }
        const f32 away =
            std::sqrt(item.position.x * item.position.x + item.position.z * item.position.z);
        if (away <= reach) {
            item.alpha = std::min(item.alpha + gain, 1.0f);
        }
        left = left || item.alpha < 1.0f;
    }
    m_revealing = left;
}

} // namespace gdl::game
