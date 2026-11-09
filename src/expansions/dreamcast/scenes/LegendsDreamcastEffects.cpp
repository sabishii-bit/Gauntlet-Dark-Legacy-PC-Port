#include <algorithm>

#include "engine/core/Log.h"

#include "expansions/shared/scenes/LegendsLevel.h"

namespace gdl::game {

bool LegendsLevel::playDreamcastOpponentSound(std::string_view name) {
    if (const auto token = m_dreamcastSounds.namedToken(name)) {
        m_opponentSounds.push_back(*token);
    } else if (!name.empty() && m_dreamcastSounds.opened() &&
               std::ranges::find(m_missingOpponentSounds, name) == m_missingOpponentSounds.end()) {
        m_missingOpponentSounds.emplace_back(name);
        log::warn("Native Dreamcast opponent cue has no authored routing: {}", name);
    }
    return true;
}

bool LegendsLevel::continueDreamcastSpeechFrom(LegendsLevel& previous) {
    if (this != &previous && platform() == LegendsPlatform::Dreamcast &&
        previous.platform() == LegendsPlatform::Dreamcast && previous.m_soundOutput &&
        (!m_soundOutput || m_soundOutput == previous.m_soundOutput) &&
        m_dreamcastSpeech.continueFrom(*previous.m_soundOutput, previous.m_dreamcastSpeech)) {
        m_soundOutput = previous.m_soundOutput;
        m_entrySpeechAwaitQueue = true;
        return true;
    }
    return false;
}

} // namespace gdl::game
