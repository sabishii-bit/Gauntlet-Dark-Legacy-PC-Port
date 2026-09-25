#include "game/screens/SwitchCutscene.h"

#include <algorithm>

#include "engine/core/Types.h"

namespace gdl::game {

bool SwitchCutscene::begin(const TriggerCameraCue& cue, const WorldLayout& layout, bool tower) {
    // These tower ids belong to Sumner and relic ceremonies, not to switches.
    if (cue.id < 0 ||
        (tower && ((cue.id >= 170 && cue.id < 184) || cue.id == 198 || cue.id > 200))) {
        return false;
    }
    const WorldLocator* marker = nullptr;
    for (const auto& locator : layout.locators()) {
        if (locator.kind == LocatorKind::TriggerCamera &&
            locator.next == static_cast<u32>(cue.id)) {
            marker = &locator; // later registrations replace earlier duplicate ids
        }
    }
    if (marker == nullptr) {
        return false;
    }
    WorldCamera camera;
    camera.position = marker->position;
    camera.pitch = marker->rotation.x;
    camera.yaw = marker->rotation.y;
    // Trigger cameras construct a look direction from yaw/pitch, without marker roll.
    m_camera = camera;
    m_target = cue.target;
    m_lead = kLeadTicks;
    m_remaining = marker->delay == 0 ? kDefaultTicks
                                     : static_cast<s32>(marker->delay) * kTicksPerDurationUnit;
    m_showing = false;
    return true;
}

void SwitchCutscene::update(s32 ticks, bool targetSettled) {
    if (!active()) {
        return;
    }
    ticks = std::max(ticks, 0);
    if (m_lead > 0) {
        m_lead = std::max(0, m_lead - ticks);
        return;
    }
    m_showing = true;
    m_remaining = std::max(0, m_remaining - ticks);
    if (m_remaining == 0 && targetSettled) {
        clear();
    }
}

void SwitchCutscene::clear() {
    m_camera.reset();
    m_target = -1;
    m_lead = 0;
    m_remaining = 0;
    m_showing = false;
}
} // namespace gdl::game
