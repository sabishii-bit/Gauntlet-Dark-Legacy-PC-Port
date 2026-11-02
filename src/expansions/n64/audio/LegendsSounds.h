#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "engine/audio/SoundPlayer.h"

#include "expansions/n64/audio/LegendsAudioMix.h"
#include "expansions/n64/audio/LegendsSoundTable.h"
#include "expansions/n64/audio/N64LegendsEffects.h"

namespace gdl::game {

struct LegendsSoundCue {
    u16 effect = 0;
    f32 volume = 1;
    f32 pan = 0;
    f32 pitch = 1;
};

/** Native Legends cue tokens and game-side defaults, above the MusyX bank.
 * Streamed speech uses a different native codec/path and is not reinterpreted
 * as an effect. This facade never substitutes Dark Legacy sound names. */
class LegendsSounds {
public:
    bool openN64(const std::filesystem::path& nativeRoot);
    bool opened() const { return !m_entries.empty() && m_effects.opened(); }
    std::optional<LegendsSoundCue> resolve(u32 token) const;
    /** Boss effects retain authored S_* strings. Native e007a408 selects the
     * first full case-insensitive name, not a GameCube sound-bank substitute. */
    std::optional<u32> namedToken(std::string_view name) const;
    SoundHandle play(SoundPlayer& output, u32 token, f32 volume = 1, bool loop = false);

private:
    N64LegendsEffects m_effects;
    std::vector<LegendsSoundEntry> m_entries;
    std::unordered_map<u16, legends_audio_mix::Calibration> m_calibrations;
};

} // namespace gdl::game
