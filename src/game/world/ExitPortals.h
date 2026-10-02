#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
#include "game/world/TowerAccess.h"

namespace gdl::game {

/** Someone who can stand on a portal. */
struct PortalVisitor {
    Vec3 position{0.0f, 0.0f, 0.0f};
    f32 radius = 0.75f;
    s32 party = -1;     ///< who it is, by place in the party
    bool still = false; ///< standing without moving
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
        bool shut = false; ///< the tower has not opened it: it wears EXIT_OFF and takes nobody
        f32 alpha = 1.0f;  ///< tower route reveal; does not change progression eligibility
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

    /** A portal the tower keeps shut, by the world and level (from nought) it leads to. */
    struct ShutGate {
        s32 world = -1;
        s32 gate = -1;
        bool operator==(const ShutGate&) const = default;
    };

    /** Stands a portal at every exit item of the layout, its figure from `items` (which must
     * outlive them), falling back to `realmItems` for missing trees; true when the level has any.
     * Both archives must outlive the portals. With `access` (the tower's), a portal the party
     * may not pass wears the EXIT_OFF figure instead and takes nobody (fn_8005B5B8). */
    bool bind(RenderDevice& device, const WorldLayout& layout, ItemArchive& items,
              const LevelCatalog& catalog, const WorldCollision* collision,
              ItemArchive* realmItems = nullptr, const TowerAccess* access = nullptr);
    void clear();
    usize size() const { return m_portals.size(); }
    const Portal& portal(usize index) const { return m_portals[index]; }
    void consume(usize index) { m_portals[index].consumed = true; }
    /** The gates of the portals bound shut, whose glows the tower puts out. */
    std::vector<ShutGate> shutGates() const;
    void setAlpha(std::string_view tag, f32 alpha);

    /** Steps every portal by `ticks` (`seconds` long); returns the portal ready to transport
     * the whole party, retaining its raised glow while the departure plays. */
    std::optional<usize> update(s32 ticks, f32 seconds, std::span<const PortalVisitor> party);
    /** Who stood still this update on a portal the rest of a party of more than one had yet
     * to reach (DoExit's wait), by place in the party. */
    std::vector<s32> takeWaiting() { return std::exchange(m_waiting, {}); }
    /** The occupied exit's position for its continuous flame, including moving visitors. */
    std::optional<Vec3> flamePosition(std::span<const PortalVisitor> party) const;
    /** Continues the selected sequences while gameplay is held for transportation. */
    void animate(f32 seconds);
    void draw(RenderDevice& device, const Mat4& clip, const WorldLighting& lighting,
              const CameraFrame* camera = nullptr,
              TreeModel::Pass pass = TreeModel::Pass::All) const;

    /** The two characters of an exit's parameters that name where it leads. */
    static std::string tagOf(const ItemInstance& instance);
    /** The level a tag's digit counts to, from nought ("g1" is 0); -1 for no digit. */
    static s32 gateOf(std::string_view tag);

private:
    void advance(Portal& portal, s32 action);
    static void shut(RenderDevice& device, Portal& portal, ItemArchive& items,
                     ItemArchive* realmItems);
    static bool standsOn(const Portal& portal, const PortalVisitor& visitor, f32 extra);

    const TreeInfo* m_tree = nullptr;
    std::array<s32, kSequences.size()> m_sequences{-1, -1, -1, -1, -1};
    std::vector<s32> m_waiting;
    std::vector<Portal> m_portals;
};

} // namespace gdl::game
