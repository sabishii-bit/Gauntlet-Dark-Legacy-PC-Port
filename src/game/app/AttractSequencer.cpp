#include "game/app/AttractSequencer.h"

namespace gdl::game {

AttractStep AttractSequencer::next() {
    const AttractEntry& entry = kScreenTable[m_position];
    if (++m_position == kScreenTable.size()) {
        m_position = 0;
        m_wave = (m_wave + 1) % kWaveCount;
    }

    AttractStep step;
    step.screen = entry.screen;
    if (entry.screen == AttractScreen::TitleMovie || entry.screen == AttractScreen::Movie) {
        step.movie = movieName(entry.variant, m_wave);
    }
    return step;
}

void AttractSequencer::reset() {
    m_position = 0;
    m_wave = 0;
}

std::string_view AttractSequencer::movieName(s32 variant, u32 wave) {
    constexpr std::array<std::string_view, kWaveCount> kCharacterMovies{"war_dwa", "val_kni",
                                                                        "wiz_sor", "jes_arc"};
    switch (variant) {
    case 0: return "midway";
    case 1: return "opening";
    case 2: return "title2";
    case 3: return kCharacterMovies[wave % kWaveCount];
    case 4: return "fail";
    default: return {};
    }
}

} // namespace gdl::game
