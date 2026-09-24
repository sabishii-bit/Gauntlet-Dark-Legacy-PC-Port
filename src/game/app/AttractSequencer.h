#pragma once

#include <array>
#include <string>
#include <string_view>

#include "engine/core/Types.h"

namespace gdl::game {

/** The screens of the pre-game attract loop, in the order the original cycles them. */
enum class AttractScreen : u8 {
    Credits = 0,
    TitleMovie = 1,
    Movie = 2,
    Instructions = 3,
    Screen2D = 4,
    Contest = 5,
    Demo = 6,
    HighScores = 7,
    Flyby = 8,
    TitleScreen = 9,
};

struct AttractEntry {
    AttractScreen screen;
    s32 variant; ///< movie table row, or a screen-specific style
};

struct AttractStep {
    AttractScreen screen = AttractScreen::Credits;
    std::string movie; ///< movie name for movie screens, empty otherwise
};

/** Walks the attract screen table; the "wave" advances each time the table wraps. */
class AttractSequencer {
public:
    static constexpr u32 kWaveCount = 4;
    static constexpr std::array<AttractEntry, 12> kScreenTable{{
        {AttractScreen::TitleMovie, 0},
        {AttractScreen::Movie, 1},
        {AttractScreen::Movie, 2},
        {AttractScreen::Screen2D, 0},
        {AttractScreen::TitleScreen, 1},
        {AttractScreen::Screen2D, 1},
        {AttractScreen::Flyby, 0},
        {AttractScreen::Movie, 3},
        {AttractScreen::Screen2D, 2},
        {AttractScreen::Flyby, 0},
        {AttractScreen::Credits, 0},
        {AttractScreen::Movie, 4},
    }};

    /** Returns the next screen of the loop. */
    AttractStep next();

    void reset();
    u32 wave() const { return m_wave; }
    usize position() const { return m_position; }

    /** Start may leave idle attract movies only after the startup sequence reaches the title. */
    bool canSkipToTitle() const { return m_titleShown; }
    void titleShown() { m_titleShown = true; }

    /** Movie played for a movie table row in a given wave. */
    static std::string_view movieName(s32 variant, u32 wave);

private:
    usize m_position = 0;
    u32 m_wave = 0;
    bool m_titleShown = false;
};

} // namespace gdl::game
