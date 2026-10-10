#pragma once

#include "game/netplay/CombatSnapshot.h"
#include "game/world/PlacedItems.h"

namespace gdl::game {

/** Load-time archive/tree roster. IDs are deterministic for matching ordered
 * assets, not pointers, filenames or packet-driven loads. Includes future drops
 * and transformed food/treasure, not just the currently visible instances.
 * Archives and their device-owned textures must outlive this roster. */
class PickupResources {
public:
    bool bind(RenderDevice& device, std::span<ItemArchive* const> archives);
    void clear() {
        m_entries.clear();
        m_archives.clear();
    }
    u32 id(const PlacedItems::Item& item) const;
    bool accepts(const PickupState& item) const;
    void draw(RenderDevice& device, const PickupState& item, const Mat4& clip,
              const WorldLighting& lighting, const CameraFrame& camera, TreeModel::Pass pass);

private:
    struct Archive {
        ItemArchive* source = nullptr;
        TextureAnimator textures;
    };
    struct Entry {
        usize archive = 0;
        const TreeInfo* tree = nullptr;
        TreeModel model;
    };
    std::vector<Archive> m_archives;
    std::vector<Entry> m_entries;
};

class PickupCapture {
public:
    /** Read-only end-of-tick sample; never collects, advances physics, or drains
     * feedback. Meshless items and particle tails are outside this visual slice.
     * Missing visible resources/capacity reject without changing the checkpoint. */
    static bool append(CombatSnapshot& snapshot, const PlacedItems& items,
                       const PickupResources& resources);
};

/** Display only: no collision, inventory, pickup judge or local item simulation. */
class ReplicaPickups {
public:
    bool begin(u64 epoch);
    void clear();
    bool show(const CombatSnapshot& snapshot, const PickupResources& resources);
    void draw(RenderDevice& device, PickupResources& resources, const Mat4& clip,
              const WorldLighting& lighting, const CameraFrame& camera,
              TreeModel::Pass pass = TreeModel::Pass::All) const;
    usize count() const { return m_items.size(); }

private:
    u64 m_epoch = 0;
    std::optional<u64> m_tick;
    std::vector<PickupState> m_items;
};
} // namespace gdl::game
