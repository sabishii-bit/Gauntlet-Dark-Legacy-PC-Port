#include "game/world/PlacedItems.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "engine/core/Log.h"

namespace gdl::game {

namespace {

/** The instance's pitch, yaw and roll as the original stacks them onto its matrix. */
Mat4 placement(const Vec3& position, const Vec3& rotation) {
    Mat4 transform = glm::translate(Mat4{1.0f}, position);
    transform = glm::rotate(transform, rotation.y, Vec3{0.0f, 1.0f, 0.0f});
    transform = glm::rotate(transform, -rotation.x, Vec3{1.0f, 0.0f, 0.0f});
    return glm::rotate(transform, rotation.z, Vec3{0.0f, 0.0f, 1.0f});
}

} // namespace

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
    m_archives.assign(archives.begin(), archives.end());
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
        item.minPlayers = instance.minPlayers;
        item.radius = info.radius;
        item.height = info.height;
        bool bound = false;
        for (ItemArchive* archive : archives) {
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
                bound = true;
                break;
            }
        }
        if (!bound) {
            log::warn("Placed items: no archive holds {}", item.name);
            continue;
        }
        item.position = instance.position;
        if (collision != nullptr) {
            if (const auto floor = collision->floorAt(instance.position, kFloorReachAbove,
                                                      kFloorReachBelow);
                floor.has_value()) {
                item.position.y = floor->y + kFloorLift;
            }
        }
        item.transform = placement(item.position, instance.rotation);
        item.visible = item.shownTo(m_players);
        m_items.push_back(std::move(item));
    }
    return !m_items.empty();
}

void PlacedItems::clear() {
    m_items.clear();
    m_effects.clear();
    m_bursts.clear();
    m_archives.clear();
    m_motions.clear();
    m_frameRemainder = 0.0f;
    m_revealTime = 0.0f;
    m_revealing = false;
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
        item.visible = !item.taken && item.shownTo(players);
    }
}

std::vector<Pickup> PlacedItems::collect(RenderDevice& device,
                                         std::span<const Collector> collectors) {
    std::vector<Pickup> pickups;
    for (usize i = 0; i < m_items.size(); ++i) {
        Item& item = m_items[i];
        if (!item.visible || item.taken) {
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
        item.taken = true;
        item.visible = false;
        Pickup pickup;
        pickup.item = i;
        pickup.collector = taker;
        pickup.subtype = item.subtype;
        pickup.realm = item.realm();
        pickup.position = item.position;
        if (pickup.realm > 0 && static_cast<usize>(pickup.realm) < kGemEffects.size()) {
            startEffect(device, kGemEffects[static_cast<usize>(pickup.realm)], item.position);
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
            ParticleDescriptor descriptor = ParticleDescriptor::fromTemplate(
                templates[static_cast<usize>(node.particle)]);
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
            std::unordered_map<u32, Vec2> offsets;
            for (usize i = 0; i < motion.animator.size(); ++i) {
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

void PlacedItems::update(f32 seconds) {
    // The archives' texture animations step once a game frame: the sheen on the crystals.
    m_frameRemainder += seconds * kFrameRate;
    const f32 whole = std::floor(m_frameRemainder);
    m_frameRemainder -= whole;
    if (whole > 0.0f) {
        for (ArchiveMotion& motion : m_motions) {
            motion.animator.step(static_cast<u32>(whole));
        }
        applyTextureMotion();
    }
    // The figures on show play their sequence over and over.
    for (Item& item : m_items) {
        if (item.visible && item.figure != nullptr && item.player.playing()) {
            item.player.advance(seconds, true);
            item.pose.evaluate(*item.figure, item.player.sequence(), item.player.frame());
            item.model.setFrame(item.player.sequence(), static_cast<s32>(item.player.frame()));
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
        return std::ranges::none_of(effect.emitters, [&](const std::pair<usize, usize>& ride) {
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
        const f32 away = std::sqrt(item.position.x * item.position.x +
                                   item.position.z * item.position.z);
        if (away <= reach) {
            item.alpha = std::min(item.alpha + gain, 1.0f);
        }
        left = left || item.alpha < 1.0f;
    }
    m_revealing = left;
}

} // namespace gdl::game
