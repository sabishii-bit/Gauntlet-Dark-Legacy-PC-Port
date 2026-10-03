#pragma once
#include <functional>

#include "engine/core/Types.h"

#include "game/config/GameConfig.h"

namespace gdl::game {
/** A reversible live video trial. Only explicit confirmation writes settings. */
class VideoSettings {
public:
    using Apply = std::function<bool(const GameConfig&)>;
    using Clock = std::function<f64()>;
    static constexpr s32 kConfirmSeconds = 15;
    void begin(const GameConfig& saved, Apply preview, Apply persist, Clock clock = {});
    bool apply(const GameConfig& draft);
    bool confirm();
    bool revert();
    bool update();
    bool pending() const { return m_pending; }
    s32 remaining() const;
    const GameConfig& saved() const { return m_saved; }

private:
    GameConfig m_saved;
    GameConfig m_trial;
    Apply m_preview;
    Apply m_persist;
    Clock m_clock;
    f64 m_deadline = 0;
    bool m_pending = false;
};
} // namespace gdl::game
