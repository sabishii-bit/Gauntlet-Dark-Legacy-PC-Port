#include "game/menu/VideoSettings.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include "engine/core/Types.h"

namespace gdl::game {
void VideoSettings::begin(const GameConfig& saved, Apply preview, Apply persist, Clock clock) {
    m_saved = saved;
    m_preview = std::move(preview);
    m_persist = std::move(persist);
    m_clock = clock ? std::move(clock) : Clock{[] {
        return std::chrono::duration<f64>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }};
    m_pending = false;
}
bool VideoSettings::apply(const GameConfig& draft) {
    if (m_pending || !m_preview || !m_preview(draft)) {
        return false;
    }
    m_trial = draft;
    m_pending = true;
    m_deadline = m_clock() + kConfirmSeconds;
    return true;
}
s32 VideoSettings::remaining() const {
    return m_pending ? static_cast<s32>(std::ceil(std::clamp(m_deadline - m_clock(), 0.0,
                                                             static_cast<f64>(kConfirmSeconds))))
                     : 0;
}
bool VideoSettings::revert() {
    if (!m_pending) {
        return true;
    }
    if (!m_preview(m_saved)) {
        return false;
    }
    m_pending = false;
    return true;
}
bool VideoSettings::confirm() {
    if (!m_pending) {
        return false;
    }
    if (remaining() == 0 || !m_persist || !m_persist(m_trial)) {
        revert();
        return false;
    }
    m_saved = m_trial;
    m_pending = false;
    return true;
}
bool VideoSettings::update() {
    return m_pending && remaining() == 0 && revert();
}
} // namespace gdl::game
