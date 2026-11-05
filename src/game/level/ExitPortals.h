#pragma once

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/assets/AssetScope.h"
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

#include "game/level/ItemFigure.h"
#include "game/level/LevelCatalog.h"
#include "game/level/TowerAccess.h"

namespace gdl::game {

/** Someone who can stand on a portal. */
struct PortalVisitor {
    Vec3 position{0.0f, 0.0f, 0.0f};
    f32 radius = 0.75f;
    s32 partyIndex = -1; ///< stable party identity, required for ordinary departure
    bool still = false;  ///< directional input released, not an animation or collision lock
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
    static constexpr s32 kIdleTicks = 6;     ///< DoExit's readiness wait, in native 60Hz ticks
    static constexpr f32 kReach = 3.0f;      ///< how far over or under a portal one counts
    static constexpr f32 kFloorLift = 0.1f;

    /** Authored contact/support values, independent of artwork and travel state. */
    struct Contact {
        s32 instance = -1;
        bool secret = false;
        s32 minPlayers = 1;
        Vec3 position{0};
        Mat4 transform{1};
        s32 support = -1;
        Mat4 supportLocal{1};
        f32 radius = 3;
        void sync(const WorldCollision* collision);
        bool touches(const PortalVisitor& visitor, f32 extra) const;
    };
    static std::vector<Contact> contacts(const WorldLayout& layout,
                                         const WorldCollision* collision);
    /** Live values, separate from the stage-retained ordinary/off model. */
    struct PortalState : Contact {
        bool consumed = false;
        bool shut = false; ///< the tower has not opened it: it wears EXIT_OFF and takes nobody
        f32 alpha = 1.0f;  ///< tower route reveal; does not change progression eligibility
        std::optional<Vec3> departurePosition;
        ItemFigure icon;
        s32 action = 0;    ///< which of kSequences it plays
        s32 ticksLeft = 0; ///< before it may move on
        TreePose pose;
        AnimationPlayer player;
    };
    /** One portal. */
    struct Portal : PortalState {
        const ItemArchive* archive = nullptr; ///< borrowed source of the ordinary/off figure
        std::string tag;                      ///< "g1"
        std::optional<LevelRef> destination;  ///< none for a tag naming no level
        TreeModel model;
    };

    /** A portal the tower keeps shut, by the world and level (from nought) it leads to. */
    struct ShutGate {
        s32 world = -1;
        s32 gate = -1;
        bool operator==(const ShutGate&) const = default;
    };

    /** Stands a portal at every exit item of the layout, its figure from `items` (which must
     * outlive them), falling back to `realmItems` for missing trees; true when the level has any.
     * Both archives and the collision world must outlive the portals. With `access` (the tower's),
     * a portal the party may not pass wears the EXIT_OFF figure instead and takes nobody
     * (fn_8005B5B8). */
    bool bind(RenderDevice& device, const WorldLayout& layout, const AssetScope& scope,
              const LevelCatalog& catalog, const WorldCollision* collision,
              const TowerAccess* access = nullptr);
    void clear();
    usize size() const { return m_portals.size(); }
    const Portal& portal(usize index) const { return m_portals[index]; }
    void consume(usize index) { m_portals[index].consumed = true; }
    /** The gates of the portals bound shut, whose glows the tower puts out. */
    std::vector<ShutGate> shutGates() const;
    void setAlpha(std::string_view tag, f32 alpha);
    /** Carries exits with their supporting floors, including during route-reveal cameras. */
    void syncFloors();

    /** Steps every portal by `ticks` (`seconds` long); returns the portal ready to transport
     * the whole party, retaining its raised glow while the departure plays. */
    std::optional<usize> update(s32 ticks, f32 seconds, std::span<const PortalVisitor> party);
    /** Who is ready on a portal the rest of a party of more than one has yet to reach
     * (DoExit's wait), by place in the party. */
    std::vector<s32> takeWaiting() { return std::exchange(m_wait.waiting, {}); }
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
    struct VisitorWait {
        s32 partyIndex = -1;
        s32 ticks = 0;
    };
    struct Waiting {
        std::vector<s32> waiting;
        std::vector<VisitorWait> visitors;
    };

public:
    class LocalSnapshot {
        friend class ExitPortals;
        std::vector<PortalState> m_portals;
        Waiting m_waiting;
        const ExitPortals* m_owner = nullptr;
        std::weak_ptr<const u8> m_binding;
    };
    void saveLocal(LocalSnapshot& saved) const;
    bool canRestoreLocal(const LocalSnapshot& saved) const;
    bool restoreLocal(const LocalSnapshot& saved);
    bool appendNativeDigest(NativeStateDigest& digest) const;
    bool appendLocalDigest(NativeStateDigest& digest, const LocalSnapshot& saved) const;
    bool registerNativeResources(NativeStateResources& resources) const;

private:
    void appendBinding(NativeStateDigest& digest) const;
    static void appendPortal(NativeStateDigest& digest, const PortalState& state);
    static void appendWaiting(NativeStateDigest& digest, const Waiting& state);
    void advance(Portal& portal, s32 action);
    bool readyToLeave(s32 partyIndex) const;
    void updateReadiness(s32 ticks, std::span<const PortalVisitor> party, f32 extra);
    static void shut(RenderDevice& device, Portal& portal, const AssetScope& scope);
    static bool standsOn(const Portal& portal, const PortalVisitor& visitor, f32 extra);

    const TreeInfo* m_tree = nullptr;
    const WorldCollision* m_collision = nullptr; ///< borrowed through clear()
    std::array<s32, kSequences.size()> m_sequences{-1, -1, -1, -1, -1};
    Waiting m_wait;
    std::vector<Portal> m_portals;
    std::shared_ptr<const u8> m_checkpointBinding = std::make_shared<const u8>(0);
};

} // namespace gdl::game
