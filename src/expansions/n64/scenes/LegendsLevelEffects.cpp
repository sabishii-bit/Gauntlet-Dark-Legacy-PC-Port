#include <algorithm>
#include <utility>

#include "engine/core/Log.h"

#include "expansions/shared/scenes/LegendsLevel.h"

namespace gdl::game {

bool LegendsLevel::playN64OpponentSound(std::string_view name) {
    if (platform() != LegendsPlatform::N64) {
        return false;
    }
    if (name == "S_LOBBER_BOMB") {
        // e005cabc assigns bomb contact action 1; e00784d8 dispatches that
        // action to the global 0x57 cue before creating the fire burst.
        m_opponentSounds.push_back(0x57U);
        return true;
    }
    if (name == "S_SUICIDE_BOMB") {
        // e005ec28 emits 0x93 except in realm 7, where e0074eec's poison
        // burst emits 0x3c. Use the same realm policy as the damaging cloud.
        const bool poison = suicideUsesGas().value_or(false);
        m_opponentSounds.push_back(poison ? 0x3cU : 0x93U);
        return true;
    }
    if (!m_boss) {
        return false;
    }
    if (const auto token = m_sounds.namedToken(name)) {
        // Boss.bin e0881b84 calls the non-spatial e007a25c with priority 8.
        // Its native effect owns pan and lifetime; GC distance curves do not apply.
        m_opponentSounds.push_back(*token);
    } else if (!name.empty() && m_sounds.opened() &&
               std::ranges::find(m_missingOpponentSounds, name) == m_missingOpponentSounds.end()) {
        m_missingOpponentSounds.emplace_back(name);
        log::warn("Native N64 boss cue has no authored routing: {}", name);
    }
    return true;
}

void LegendsLevel::continueN64SpeechFrom(LegendsLevel& previous) {
    if (this == &previous || platform() != LegendsPlatform::N64 ||
        previous.platform() != LegendsPlatform::N64 || !m_speech.opened() ||
        !previous.m_speech.opened() || !previous.m_destination || !previous.m_exitSpeechPlayed ||
        !previous.m_soundOutput) {
        return;
    }
    if (m_soundOutput && m_soundOutput != previous.m_soundOutput) {
        return;
    }
    // Both scenes use the app's long-lived SoundPlayer. Move its narration
    // owner, not the stream cursor or the already-started phrase. Explicitly
    // empty the source: default moves alone would leave its scalar handle live.
    m_speech.stop(*previous.m_soundOutput);
    m_speech = std::exchange(previous.m_speech, {});
    m_soundOutput = previous.m_soundOutput;
    m_entrySpeechAwaitQueue = true;
}

} // namespace gdl::game
