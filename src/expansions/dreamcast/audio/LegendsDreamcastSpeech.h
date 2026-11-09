#pragma once

#include <span>
#include <vector>

#include "expansions/dreamcast/audio/LegendsDreamcastSounds.h"
#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"

namespace gdl::game {

/** Native speech uses the announcer queue, not spatial fixture attenuation.
 * SoundPlayer owns queue advancement; decoded samples remain with this owner
 * while a completed level is parked behind results/shop menus. */
class LegendsDreamcastSpeech {
public:
    bool open(const std::filesystem::path& nativeRoot);
    void setEntry(const formats::LegendsDreamcastFixtureScope& scope);
    const std::vector<u32>& entryCue() const { return m_entry; }
    bool enqueue(SoundPlayer& output, std::span<const u32> tokens);
    bool playing(const SoundPlayer& output) const;
    void stop(SoundPlayer& output);
    /** Keeps this level's entry cue, adopting only the prior narration owner. */
    bool continueFrom(SoundPlayer& output, LegendsDreamcastSpeech& previous);

    /** AudioNumRunesFound 8c0de200 and the 8c1272f8 number table. */
    static std::vector<u32> runestoneCue(u32 count);

private:
    std::filesystem::path m_root;
    LegendsDreamcastSounds m_sounds;
    std::vector<u32> m_entry;
    std::vector<SoundHandle> m_handles;
};

} // namespace gdl::game
