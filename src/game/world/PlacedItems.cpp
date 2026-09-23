#include "game/world/PlacedItems.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "engine/core/Log.h"

#include "game/world/ItemFigure.h"

namespace gdl::game {

bool PlacedItems::Item::shownTo(std::int32_t players) const {
    if (minPlayers > kExactPlayersMark) {
        return players == minPlayers - kExactPlayersMark;
    }
    return players >= minPlayers;
}

/** Within the two radii sideways and the item's height plus the collector's slack up or
 * down. */
bool PlacedItems::Item::touchedBy(const Collector& collector) const {
    const Vec3 away = collector.position - position;
    const float reach = radius + collector.radius;
    if (away.x * away.x + away.z * away.z > reach * reach) {
        return false;
    }
    return std::abs(away.y) <= height + collector.height;
}

std::int32_t PlacedItems::Item::realm() const {
    if (subtype != ItemInfo::kCrystal || value < 0 ||
        static_cast<std::size_t>(value) >= kCrystalRealms.size()) {
        return -1;
    }
    return kCrystalRealms[static_cast<std::size_t>(value)];
}

bool PlacedItems::bind(RenderDevice& device, const WorldLayout& layout,
                       const WorldCollision* collision, std::span<ItemArchive* const> archives) {
    clear();
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
    for (std::size_t index = 0; index < instances.size(); ++index) {
        const ItemInstance& instance = instances[index];
        if (instance.info < 0 || static_cast<std::size_t>(instance.info) >= infos.size()) {
            continue;
        }
        const ItemInfo& info = infos[static_cast<std::size_t>(instance.info)];
        if (info.type != ItemInfo::kPowerup) {
            continue;
        }
        Item item;
        item.name = instance.name.empty() ? info.name : instance.name;
        item.instance = static_cast<std::int32_t>(index);
        item.info = instance.info;
        item.subtype = info.subtype;
        item.value = info.value;
        if (info.subtype == ItemInfo::kScroll) {
            // A scroll's page is the instance's, from one.
            item.value = static_cast<std::int16_t>(instance.params[0] | (instance.params[1] << 8));
        }
        item.flags = info.properties;
        item.strength = static_cast<float>(info.activeOn);
        item.minPlayers = instance.minPlayers;
        item.radius = info.radius;
        item.height = info.height;
        if (!makeFigure(device, item)) {
            log::warn("Placed items: no archive holds {}", item.name);
            continue;
        }
        item.position = instance.position;
        if (collision != nullptr) {
            if (const auto floor =
                    collision->floorAt(instance.position, kFloorReachAbove, kFloorReachBelow);
                floor.has_value()) {
                item.position.y = floor->y + kFloorLift;
            }
        }
        item.transform = itemPlacement(item.position, instance.rotation);
        item.visible = item.shownTo(m_players);
        m_items.push_back(std::move(item));
    }
    return !m_items.empty();
}

void PlacedItems::clear() {
    m_infos.clear();
    m_items.clear();
    m_effects.clear();
    m_bursts.clear();
    m_archives.clear();
    m_motions.clear();
    m_collision = nullptr;
    m_frameRemainder = 0.0f;
    m_revealTime = 0.0f;
    m_revealing = false;
}

std::size_t PlacedItems::visibleCount() const {
    std::size_t count = 0;
    for (const Item& item : m_items) {
        count += item.visible ? 1 : 0;
    }
    return count;
}

void PlacedItems::setPlayerCount(std::int32_t players) {
    m_players = players;
    for (Item& item : m_items) {
        item.visible = !item.taken && item.shownTo(players);
    }
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
    return placeRecord(device, static_cast<std::int32_t>(info - m_infos.begin()), position,
                       collision);
}

bool PlacedItems::placeRecord(RenderDevice& device, std::int32_t record, const Vec3& position,
                              const WorldCollision* collision, std::int32_t amount) {
    if (record < 0 || static_cast<std::size_t>(record) >= m_infos.size() ||
        m_infos[static_cast<std::size_t>(record)].type != ItemInfo::kPowerup) {
        return false;
    }
    const ItemInfo* info = &m_infos[static_cast<std::size_t>(record)];
    Item item;
    item.name = info->name;
    item.info = record;
    item.subtype = info->subtype;
    item.value = amount > 0 ? amount : info->value;
    item.flags = info->properties;
    item.strength = static_cast<float>(info->activeOn);
    item.radius = info->radius;
    item.height = info->height;
    if (!makeFigure(device, item)) {
        log::warn("Placed items: no archive holds {}", item.name);
        return false;
    }
    item.position = position;
    if (collision != nullptr) {
        if (const auto floor = collision->floorAt(position, kFloorReachAbove, kFloorReachBelow);
            floor.has_value()) {
            item.position.y = floor->y + kFloorLift;
        }
    }
    item.transform = itemPlacement(item.position, Vec3{0.0f, 0.0f, 0.0f});
    item.visible = true;
    m_items.push_back(std::move(item));
    return true;
}

/** Placed where it starts (not on the floor) and set flying; with no floor to land on it
 * stays where it is thrown. */
bool PlacedItems::throwItem(RenderDevice& device, std::string_view name, const Vec3& position,
                            const Vec3& velocity, const WorldCollision* collision,
                            float noGrabSeconds) {
    if (!place(device, name, position, collision)) {
        return false;
    }
    Item& item = m_items.back();
    item.noGrabSeconds = noGrabSeconds;
    if (collision != nullptr) {
        item.position = position;
        item.transform = itemPlacement(item.position, Vec3{0.0f, 0.0f, 0.0f});
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
    for (std::size_t i = 0; i < m_items.size(); ++i) {
        Item& item = m_items[i];
        if (!item.takeable()) {
            continue;
        }
        std::size_t taker = collectors.size();
        for (std::size_t c = 0; c < collectors.size() && taker == collectors.size(); ++c) {
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
        if (judge) {
            const std::optional<std::int32_t> left = judge(pickup);
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
        if (pickup.realm > 0 && static_cast<std::size_t>(pickup.realm) < kGemEffects.size()) {
            startEffect(device, kGemEffects[static_cast<std::size_t>(pickup.realm)], item.position);
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
        for (std::size_t n = 0; n < figure.nodes.size(); ++n) {
            const TreeNodeInfo& node = figure.nodes[n];
            if (node.particle < 0 || static_cast<std::size_t>(node.particle) >= templates.size()) {
                continue;
            }
            ParticleDescriptor descriptor = ParticleDescriptor::fromTemplate(
                templates[static_cast<std::size_t>(node.particle)]);
            // A node's own direction stands in when the template names none.
            if (!templates[static_cast<std::size_t>(node.particle)].sets(
                    ParticleTemplate::kDirection) &&
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
                                  static_cast<std::uint32_t>(m_effects.size() * 8 + n + 1)));
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
            std::unordered_map<std::uint32_t, Vec2> offsets;
            for (std::size_t i = 0; i < motion.animator.size(); ++i) {
                const TextureMotion shown = motion.animator.motion(i);
                if (shown.frame != nullptr) {
                    item.model.setTextureFrame(shown.slot, shown.frame);
                } else {
                    offsets[shown.slot] += shown.offset;
                }
            }
            for (const auto& [slot, offset] : offsets) {
                item.model.setTextureOffset(slot, offset);
            }
        }
    }
}

/** A thrown item's flight, as the original's coins fly: it falls under gravity, and on
 * touching the floor (where a placed item rests) bounces back at a share of its speed
 * until a bounce would not clear the touching-down height, when it stays down; sideways it
 * slows a little in the air and much more on touching down, and stops once it has all but
 * stopped. */
void PlacedItems::fly(Item& item, float seconds) {
    item.position += item.velocity * seconds;
    float over = kThrownFloorReach;
    float drag = kAirDrag * seconds;
    if (item.velocity.y <= 0.0f && m_collision != nullptr) {
        const auto floor = m_collision->floorAt(item.position, kFloorReachAbove, kThrownFloorReach);
        if (!floor.has_value()) {
            // Falling with no floor under it, it has left the level and is lost.
            item.visible = false;
            item.taken = true;
            item.thrown = false;
            return;
        }
        const float rest = floor->y + kFloorLift;
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
    if (over >= kRestHeight) {
        item.velocity.y -= kGravity * seconds;
    }
    const auto slow = [drag](float& v) { v = std::abs(v) > drag ? v - drag * v : 0.0f; };
    slow(item.velocity.x);
    slow(item.velocity.z);
    item.transform = itemPlacement(item.position, Vec3{0.0f, 0.0f, 0.0f});
    if (item.velocity == Vec3{0.0f, 0.0f, 0.0f}) {
        item.thrown = false;
    }
}

void PlacedItems::update(float seconds) {
    // The archives' texture animations step once a game frame: the sheen on the crystals.
    m_frameRemainder += seconds * kFrameRate;
    const float whole = std::floor(m_frameRemainder);
    m_frameRemainder -= whole;
    if (whole > 0.0f) {
        for (ArchiveMotion& motion : m_motions) {
            motion.animator.step(static_cast<std::uint32_t>(whole));
        }
        applyTextureMotion();
    }
    // The figures on show play their sequence over and over.
    for (Item& item : m_items) {
        if (item.visible && item.figure != nullptr && item.player.playing()) {
            item.player.advance(seconds, true);
            item.pose.evaluate(*item.figure, item.player.sequence(), item.player.frame());
            item.model.setFrame(item.player.sequence(),
                                static_cast<std::int32_t>(item.player.frame()));
        }
        item.noGrabSeconds = std::max(item.noGrabSeconds - seconds, 0.0f);
        if (item.thrown) {
            fly(item, seconds);
        }
    }
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
        return std::ranges::none_of(effect.emitters,
                                    [&](const std::pair<std::size_t, std::size_t>& ride) {
                                        return m_bursts.active(ride.second);
                                    });
    });
    if (m_effects.empty()) {
        m_bursts.prune();
    }
}

void PlacedItems::draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
                       const CameraFrame* camera) const {
    for (const Item& item : m_items) {
        if (item.visible) {
            item.model.draw(device, clip, item.transform, lighting, item.pose.matrices(), camera,
                            item.alpha);
        }
    }
    const CameraFrame frame = camera != nullptr ? *camera : CameraFrame{};
    m_bursts.draw(device, clip, frame.right, frame.up);
}

void PlacedItems::hideCrystals() {
    m_revealTime = 0.0f;
    m_revealing = false;
    for (Item& item : m_items) {
        if (item.subtype == ItemInfo::kCrystal) {
            item.alpha = 0.0f;
            m_revealing = true;
        }
    }
}

/** The reveal spreads from the world's origin at its pace; a crystal it has reached gains a
 * step of alpha a frame until it is whole. */
void PlacedItems::reveal(float seconds) {
    if (!m_revealing) {
        return;
    }
    m_revealTime += seconds;
    const float reach = kRevealSpread * (kRevealLead + m_revealTime);
    const float gain = seconds * kFrameRate * kRevealStep;
    bool left = false;
    for (Item& item : m_items) {
        if (item.subtype != ItemInfo::kCrystal || item.alpha >= 1.0f) {
            continue;
        }
        const float away =
            std::sqrt(item.position.x * item.position.x + item.position.z * item.position.z);
        if (away <= reach) {
            item.alpha = std::min(item.alpha + gain, 1.0f);
        }
        left = left || item.alpha < 1.0f;
    }
    m_revealing = left;
}

} // namespace gdl::game
