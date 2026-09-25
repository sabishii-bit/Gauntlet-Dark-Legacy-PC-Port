#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets/ItemArchive.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/world/AnimationPlayer.h"
#include "engine/world/TreeModel.h"
#include "engine/world/TreePose.h"
#include "engine/world/WorldCollision.h"
#include "engine/world/WorldLighting.h"

#include "game/world/ItemFigure.h"
#include "game/world/LevelCatalog.h"

namespace gdl::game {

/** Someone who can stand on a portal. */
struct PortalVisitor {
    Vec3 position{0.0f, 0.0f, 0.0f};
    f32 radius = 0.75f;
};

/**
 * A level's exit portals, worked the way the original works them: each is one of the level's
 * exit items, carrying the two characters that name where it leads, and wakes through its
 * five sequences (idle, ready, startup, raised glow and closing). The raised glow loops
 * while occupied, including the committed departure; left alone it closes back to idle.
 */
class ExitPortals {
public:
    static constexpr s32 kExitItem = 9;
    static constexpr s32 kSecretSubtype = 50;
    static constexpr std::string_view kFigure = "EXIT_PORTAL";
    static constexpr std::array<std::string_view, 5> kSequences{"IDLE", "READY", "ACTIVE1",
                                                                "ACTIVE2", "ACTIVE3"};
    static constexpr s32 kWaiting = 3; ///< the sequence a portal holds at for stragglers
    static constexpr s32 kLast = 4;
    static constexpr s32 kWaitingTicks = 45; ///< how long the waiting sequence holds
    static constexpr f32 kReach = 3.0f;      ///< how far over or under a portal one counts
    static constexpr f32 kFloorLift = 0.1f;

    /** One portal. */
    struct Portal {
        s32 instance = -1;
        bool secret = false;
        bool consumed = false;
        std::optional<Vec3> departurePosition;
        s32 minPlayers = 1;
        ItemFigure icon;
        Vec3 position{0.0f, 0.0f, 0.0f};
        Mat4 transform{1.0f};
        f32 radius = 3.0f;
        std::string tag;                     ///< "g1"
        std::optional<LevelRef> destination; ///< none for a tag naming no level
        s32 action = 0;                      ///< which of kSequences it plays
        s32 ticksLeft = 0;                   ///< before it may move on
        TreeModel model;
        TreePose pose;
        AnimationPlayer player;
    };

    /** Stands a portal at every exit item of the layout, its figure from `items` (which must
     * outlive them); true when the level has any. */
    bool bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items,
              const LevelCatalog& catalog, const WorldCollision* collision);
    void clear();
    usize size() const { return m_portals.size(); }
    const Portal& portal(usize index) const { return m_portals[index]; }
    void consume(usize index) { m_portals[index].consumed = true; }

    /** Steps every portal by `ticks` (`seconds` long); returns the portal ready to transport
     * the whole party, retaining its raised glow while the departure plays. */
    std::optional<usize> update(s32 ticks, f32 seconds, std::span<const PortalVisitor> party);
    /** Continues the selected sequences while gameplay is held for transportation. */
    void animate(f32 seconds);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting) const;

    /** The two characters of an exit's parameters that name where it leads. */
    static std::string tagOf(const ItemInstance& instance);

private:
    void advance(Portal& portal, s32 action);
    static bool standsOn(const Portal& portal, const PortalVisitor& visitor, f32 extra);

    const TreeInfo* m_tree = nullptr;
    std::array<s32, kSequences.size()> m_sequences{-1, -1, -1, -1, -1};
    std::vector<Portal> m_portals;
};

} // namespace gdl::game
