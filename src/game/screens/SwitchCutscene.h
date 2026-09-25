#pragma once

#include <optional>

#include "engine/assets/WorldLayout.h"
#include "engine/core/Types.h"
#include "engine/world/WorldCamera.h"

#include "game/world/LevelTriggers.h"

namespace gdl::game {

/** A switch camera holds gameplay from activation through its target's completion.
 * The lead-in keeps the previous view; only the actual shot is letterboxed. */
class SwitchCutscene {
public:
    static constexpr s32 kLeadTicks = 30;
    static constexpr s32 kDefaultTicks = 40;
    static constexpr s32 kTicksPerDurationUnit = 6;

    bool begin(const TriggerCameraCue& cue, const WorldLayout& layout, bool tower);
    void update(s32 ticks, bool targetSettled);
    void clear();
    bool active() const { return m_camera.has_value(); }
    bool showing() const { return active() && m_showing; }
    const std::optional<WorldCamera>& camera() const { return m_camera; }
    s32 target() const { return m_target; }

private:
    std::optional<WorldCamera> m_camera;
    s32 m_target = -1;
    s32 m_lead = 0;
    s32 m_remaining = 0;
    bool m_showing = false;
};
} // namespace gdl::game
