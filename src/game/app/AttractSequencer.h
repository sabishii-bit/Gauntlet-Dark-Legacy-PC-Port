#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace gdl::game {

/** The screens of the pre-game attract loop, in the order the original cycles them. */
enum class AttractScreen : std::uint8_t {
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
    int variant; ///< movie table row, or a screen-specific style
};

struct AttractStep {
    AttractScreen screen = AttractScreen::Credits;
    std::string movie; ///< movie name for movie screens, empty otherwise
};

/** Walks the attract screen table; the "wave" advances each time the table wraps. */
class AttractSequencer {
public:
    static constexpr unsigned int kWaveCount = 4;
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
    unsigned int wave() const { return m_wave; }
    std::size_t position() const { return m_position; }

    /** Movie played for a movie table row in a given wave. */
    static std::string_view movieName(int variant, unsigned int wave);

private:
    std::size_t m_position = 0;
    unsigned int m_wave = 0;
};

} // namespace gdl::game
