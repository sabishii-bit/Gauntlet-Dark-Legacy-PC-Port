#pragma once

#include <span>
#include <string>
#include <string_view>

#include "engine/core/Types.h"
#include "engine/ui/TextPainter.h"

namespace gdl::game {

struct PlayerRuntime;

/** Retail gamemain.c::fn_800521E8 (0x800521E8): the timed defeat caption.
 * The scene owns the background and plays the emitted voice cue. */
class GameOver {
public:
    static constexpr s32 kVoiceTick = 60;
    static constexpr s32 kLetterTicks = 8;
    static constexpr s32 kDurationTicks = 240;
    static constexpr s32 kTextY = 120;
    static constexpr f32 kTextScale = 2.0f;
    static constexpr std::string_view kVoice = "S_GAMEOVERVOX";
    static constexpr std::string_view kMessage = "GAME_OVER";
    static constexpr std::string_view kTextId = "gameOver.caption";

    /** An empty party is not a defeat; every participant's death must have finished. */
    static bool ready(std::span<const PlayerRuntime> players);

    void begin(std::string_view caption);
    void clear();
    /** Returns true only on the tick that crosses the announcement's start. */
    bool step(s32 ticks);
    bool active() const { return m_active; }
    bool finished() const { return m_active && m_ticks >= kDurationTicks; }
    usize letters() const;
    void draw(Canvas& canvas, const TextPainter& text, f32 width) const;

private:
    std::string m_caption;
    s32 m_ticks = 0;
    bool m_active = false;
};

} // namespace gdl::game
