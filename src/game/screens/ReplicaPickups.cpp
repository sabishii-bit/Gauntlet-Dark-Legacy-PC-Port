#include "game/screens/ReplicaPickups.h"

#include <algorithm>

namespace gdl::game {
bool PickupResources::bind(RenderDevice& device, std::span<ItemArchive* const> archives) {
    PickupResources next;
    for (auto* source : archives) {
        if (source == nullptr || !source->loaded()) {
            return false;
        }
        if (std::ranges::any_of(next.m_archives,
                                [&](const auto& entry) { return entry.source == source; }) ||
            next.m_entries.size() + source->trees.size() > 65535) {
            return false;
        }
        Archive archive;
        archive.source = source;
        archive.textures.bind(source->trees.textureAnimations(), source->textures, device);
        const auto index = next.m_archives.size();
        next.m_archives.push_back(std::move(archive));
        for (usize tree = 0; tree < source->trees.size(); ++tree) {
            Entry entry;
            entry.archive = index;
            entry.tree = &source->trees.tree(static_cast<u32>(tree));
            // Preserve the ID even for a tree without drawable mesh nodes.
            entry.model.bind(*entry.tree, source->models, source->textures, device);
            next.m_entries.push_back(std::move(entry));
        }
    }
    *this = std::move(next);
    return true;
}
u32 PickupResources::id(const PlacedItems::Item& item) const {
    for (usize index = 0; index < m_entries.size(); ++index) {
        const auto& entry = m_entries[index];
        if (entry.tree == item.figure && m_archives[entry.archive].source == item.archive) {
            return static_cast<u32>(index + 1);
        }
    }
    return 0;
}
bool PickupResources::accepts(const PickupState& item) const {
    if (!item.valid() || item.resource > m_entries.size()) {
        return false;
    }
    const auto& entry = m_entries[item.resource - 1];
    if (!entry.model.bound()) {
        return false;
    }
    const auto& animation = item.animation;
    return animation.generation == 0 ||
           (animation.sequence < entry.tree->sequences.size() &&
            item.animation.frame <= static_cast<f32>(std::max(
                                        0, entry.tree->sequences[animation.sequence].frames - 1)));
}
void PickupResources::draw(RenderDevice& device, const PickupState& item, const Mat4& clip,
                           const WorldLighting& lighting, const CameraFrame& camera,
                           TreeModel::Pass pass) {
    if (!accepts(item)) {
        return;
    }
    auto& entry = m_entries[item.resource - 1];
    const auto& animation = item.animation;
    TreePose pose;
    if (animation.generation == 0) {
        pose.rest(*entry.tree);
    } else {
        pose.evaluate(*entry.tree, animation.sequence, animation.frame, false, true);
    }
    m_archives[entry.archive].textures.apply(entry.model, *entry.tree, animation.sequence,
                                             animation.frame, item.textureFrame);
    entry.model.setPresentationFrame(animation.sequence, animation.frame);
    entry.model.draw(device, clip, item.placement, lighting, pose.matrices(), &camera, item.alpha,
                     pass);
}
bool PickupCapture::append(CombatSnapshot& snapshot, const PlacedItems& items,
                           const PickupResources& resources) {
    std::vector<PickupState> visible;
    for (usize index = 0; index < items.size(); ++index) {
        const auto& item = items.item(index);
        if (!item.visible || !item.model.bound()) {
            continue;
        }
        PickupState state;
        // PlacedItems never erases/reuses an index until clear()/the next epoch.
        state.instance = index + 1;
        state.resource = resources.id(item);
        state.continuity = item.continuity;
        state.placement = item.transform;
        state.alpha = item.alpha;
        state.textureFrame = items.textureFrame(item);
        if (item.player.playing()) {
            state.animation = {0, item.player.sequence(), item.player.generation(),
                               item.player.frame(), 1};
        }
        if (visible.size() >= CombatSnapshot::kMaxPickups || !resources.accepts(state)) {
            return false;
        }
        visible.push_back(state);
    }
    auto candidate = snapshot;
    candidate.pickups = std::move(visible);
    if (!candidate.valid()) {
        return false;
    }
    snapshot = std::move(candidate);
    return true;
}
bool ReplicaPickups::begin(u64 epoch) {
    if (epoch == 0 || epoch <= m_epoch) {
        return false;
    }
    m_epoch = epoch;
    m_tick.reset();
    m_items.clear();
    return true;
}
void ReplicaPickups::clear() {
    m_epoch = 0;
    m_tick.reset();
    m_items.clear();
}
bool ReplicaPickups::show(const CombatSnapshot& snapshot, const PickupResources& resources) {
    if (m_epoch == 0 || snapshot.motion.epoch != m_epoch || !snapshot.valid() ||
        (m_tick && snapshot.motion.tick < *m_tick) ||
        !std::ranges::all_of(snapshot.pickups,
                             [&](const auto& item) { return resources.accepts(item); })) {
        return false;
    }
    m_tick = snapshot.motion.tick;
    m_items = snapshot.pickups;
    return true;
}
void ReplicaPickups::draw(RenderDevice& device, PickupResources& resources, const Mat4& clip,
                          const WorldLighting& lighting, const CameraFrame& camera,
                          TreeModel::Pass pass) const {
    for (const auto& item : m_items) {
        resources.draw(device, item, clip, lighting, camera, pass);
    }
}
} // namespace gdl::game
