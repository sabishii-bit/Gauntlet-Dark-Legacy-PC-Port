#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/world/WorldCollision.h"

#include "expansions/n64/level/LegendsSwitches.h"
#include "expansions/shared/formats/LegendsMap.h"
#include "expansions/shared/level/LegendsFixtureEvents.h"
#include "expansions/shared/level/LegendsPortalVisitor.h"

namespace gdl::game {

/** Binds native switch targets to the same world-object identities used by
 * rendering and collision. Native triangles are already in world space, so
 * their moving transforms are deltas, not a second authored placement. */
class LegendsWorldMotion {
public:
    LegendsWorldMotion() = default;
    LegendsWorldMotion(const formats::LegendsMap& map,
                       std::span<const formats::LegendsN64Switch> definitions,
                       u8 activationThreshold = 1);
    void capturePresentation() { m_motion.capturePresentation(); }
    void update(f32 seconds);
    std::vector<LegendsSoundLoop> soundLoops() const;
    std::vector<LegendsSoundEvent> takeSounds();
    void touch(std::span<const LegendsPortalVisitor> visitors);
    bool activate(u32 nativeIndex) { return m_motion.activate(nativeIndex); }
    bool activated(u32 nativeIndex) const;
    std::optional<Vec3> platePosition(u32 nativeIndex) const;
    void bindCollision(WorldCollision& collision) const;
    void syncCollision(WorldCollision& collision) const;
    Vec3 offset(usize object, f32 alpha = -1) const;
    f32 opacity(usize object, f32 alpha = -1) const;
    /** Current floor owners of other living actors, supplied by their existing
     * collision solver. No proximity-based support is inferred. */
    void support(std::span<const s32> owners);
    /** A carried fixture follows its selected floor owner, not that owner's
     * render-parent chain. Native runtime trap+4 stores precisely this ID. */
    Vec3 supportingOffset(s32 owner, f32 alpha = -1) const {
        return {0, targetOffset(owner, alpha), 0};
    }
    const LegendsSwitches& motion() const { return m_motion; }
    const std::vector<std::string>& warnings() const { return m_warnings; }

private:
    LegendsSwitches m_motion;
    f32 targetOffset(s32 object, f32 alpha) const;
    f32 collisionOffset(s32 object, f32 alpha = -1) const;
    std::vector<s32> m_collisionObjects;
    std::vector<s32> m_fadingObjects;
    std::vector<s32> m_supportingObjects;
    std::vector<s32> m_objectTargets;
    std::vector<s32> m_objectParents;
    std::vector<s32> m_plateOwners;
    std::vector<Vec3> m_soundPositions;
    std::vector<bool> m_sounding;
    std::vector<LegendsSoundEvent> m_soundEvents;
    std::vector<std::string> m_warnings;
};

} // namespace gdl::game
