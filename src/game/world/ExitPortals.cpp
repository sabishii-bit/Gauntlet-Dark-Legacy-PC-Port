#include "game/world/ExitPortals.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Log.h"

namespace gdl::game {

namespace {

constexpr f32 kTicksPerSecond = 60.0f;
constexpr f32 kFloorReachAbove = 0.5f;
constexpr f32 kFloorReachBelow = 3.0f;
constexpr usize kTagAt = 4; ///< where an exit's parameters keep its two characters

} // namespace

std::string ExitPortals::tagOf(const ItemInstance& instance) {
    std::string tag;
    for (usize i = kTagAt; i < kTagAt + 2 && i < instance.params.size(); ++i) {
        if (instance.params[i] != 0) {
            tag.push_back(static_cast<char>(instance.params[i]));
        }
    }
    return tag;
}

bool ExitPortals::bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items,
                       const LevelCatalog& catalog, const WorldCollision* collision) {
    clear();
    const auto tree = items.loaded() ? items.trees.find(kFigure) : std::nullopt;
    if (tree.has_value()) {
        m_tree = &items.trees.tree(*tree);
        for (usize i = 0; i < kSequences.size(); ++i) {
            const auto sequence = m_tree->findSequence(kSequences[i]);
            m_sequences[i] = sequence.has_value() ? static_cast<s32>(*sequence) : -1;
        }
    }
    const std::vector<ItemInfo>& infos = layout.itemInfos();
    const std::vector<ItemInstance>& instances = layout.itemInstances();
    for (usize index = 0; index < instances.size(); ++index) {
        const ItemInstance& instance = instances[index];
        if (instance.info < 0 || static_cast<usize>(instance.info) >= infos.size() ||
            infos[static_cast<usize>(instance.info)].type != kExitItem) {
            continue;
        }
        const ItemInfo& info = infos[static_cast<usize>(instance.info)];
        Portal portal;
        portal.instance = static_cast<s32>(index);
        portal.radius = info.radius;
        portal.tag = tagOf(instance);
        portal.destination = catalog.byTag(portal.tag);
        portal.position = instance.position;
        if (collision != nullptr) {
            if (const auto floor =
                    collision->floorAt(instance.position, kFloorReachAbove, kFloorReachBelow);
                floor.has_value()) {
                portal.position.y = floor->y + kFloorLift;
            }
        }
        portal.transform = glm::rotate(glm::translate(Mat4{1.0f}, portal.position),
                                       instance.rotation.y, Vec3{0.0f, 1.0f, 0.0f});
        if (m_tree != nullptr &&
            portal.model.bind(*m_tree, items.models, items.textures, device)) {
            portal.pose.rest(*m_tree);
        }
        m_portals.push_back(std::move(portal));
    }
    for (Portal& portal : m_portals) {
        advance(portal, 0);
        portal.ticksLeft = 0;
    }
    if (!m_portals.empty() && m_tree == nullptr) {
        log::warn("Exit portals: no {} figure; the level's {} exits work unseen", kFigure,
                  m_portals.size());
    }
    return !m_portals.empty();
}

void ExitPortals::clear() {
    m_portals.clear();
    m_tree = nullptr;
    m_sequences.fill(-1);
}

/** Starts a portal's sequence; it may not move on until the sequence has played (the waiting
 * one holds its own time). */
void ExitPortals::advance(Portal& portal, s32 action) {
    portal.action = std::clamp(action, 0, kLast);
    portal.ticksLeft = 0;
    const s32 sequence = m_sequences[static_cast<usize>(portal.action)];
    if (m_tree == nullptr || sequence < 0) {
        return;
    }
    const TreeSequenceInfo& info = m_tree->sequences[static_cast<usize>(sequence)];
    portal.player.start(info, static_cast<u32>(sequence));
    portal.pose.evaluate(*m_tree, static_cast<u32>(sequence), 0.0f);
    portal.model.setFrame(static_cast<u32>(sequence), 0);
    const f32 rate = info.frameRate > 0 ? static_cast<f32>(info.frameRate)
                                        : AnimationPlayer::kDefaultRate;
    const f32 seconds = static_cast<f32>(info.frames) * rate * AnimationPlayer::kRateUnit;
    portal.ticksLeft = portal.action == kWaiting
                           ? kWaitingTicks
                           : static_cast<s32>(std::ceil(seconds * kTicksPerSecond));
}

bool ExitPortals::standsOn(const Portal& portal, const PortalVisitor& visitor, f32 extra) {
    const Vec3 away = visitor.position - portal.position;
    const f32 reach = portal.radius + extra + visitor.radius;
    return away.x * away.x + away.z * away.z <= reach * reach && std::abs(away.y) <= kReach;
}

std::optional<usize> ExitPortals::update(s32 ticks, f32 seconds,
                                         std::span<const PortalVisitor> party) {
    std::optional<usize> left;
    // A larger party is given a wider portal: a unit more for each member past the first.
    const f32 extra = party.empty() ? 0.0f : static_cast<f32>(party.size() - 1);
    for (usize index = 0; index < m_portals.size(); ++index) {
        Portal& portal = m_portals[index];
        const auto on = static_cast<usize>(std::ranges::count_if(
            party, [&](const PortalVisitor& visitor) { return standsOn(portal, visitor, extra); }));
        const bool everyone = !party.empty() && on == party.size();
        const bool ready = portal.ticksLeft <= 0;
        if (everyone) {
            if (portal.action == kLast && ready) {
                left = index;
            } else if (ready || portal.action == 0) {
                advance(portal, portal.action + 1);
            }
        } else if (on > 0) {
            // Some of the party: up to the waiting sequence, and back round from past it.
            if (ready && portal.action < kWaiting) {
                advance(portal, portal.action + 1);
            } else if (ready && portal.action > kWaiting) {
                advance(portal, 0);
            }
        } else if (portal.action > 0) {
            // Left alone it plays itself out, the waiting sequence cut short.
            if (ready || portal.action == kWaiting) {
                advance(portal, portal.action < kLast ? portal.action + 1 : 0);
            }
        }
        portal.ticksLeft = std::max(portal.ticksLeft - ticks, 0);
        if (m_tree != nullptr && portal.player.playing()) {
            const bool loops = portal.action == 0 || portal.action == 1 ||
                               portal.action == kWaiting;
            portal.player.advance(seconds, loops);
            portal.pose.evaluate(*m_tree, portal.player.sequence(), portal.player.frame());
            portal.model.setFrame(portal.player.sequence(),
                                  static_cast<s32>(portal.player.frame()));
        }
    }
    return left;
}

void ExitPortals::draw(RenderDevice& device, const Mat4& clip,
                       const WorldLighting& lighting) const {
    for (const Portal& portal : m_portals) {
        if (portal.model.bound()) {
            portal.model.draw(device, clip, portal.transform, lighting, portal.pose.matrices());
        }
    }
}

} // namespace gdl::game
