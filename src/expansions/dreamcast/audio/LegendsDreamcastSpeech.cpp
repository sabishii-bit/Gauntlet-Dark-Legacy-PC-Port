#include "expansions/dreamcast/audio/LegendsDreamcastSpeech.h"

#include <algorithm>
#include <utility>

namespace gdl::game {

bool LegendsDreamcastSpeech::open(const std::filesystem::path& nativeRoot) {
    // Only called before playback. A running owner is transferred, never reopened.
    m_root = nativeRoot.lexically_normal();
    m_entry.clear();
    m_handles.clear();
    return m_sounds.open(m_root);
}

void LegendsDreamcastSpeech::setEntry(const formats::LegendsDreamcastFixtureScope& scope) {
    m_entry.clear();
    const auto prefix = m_sounds.namedToken(scope.entrySound);
    const auto name = m_sounds.namedToken(scope.stageNameSound);
    // AudioEnterNextStage 8c0de0a0 requires both resolved cues; no fabricated
    // fallback, truncated sentence, or GameCube bank when optional audio is absent.
    if (prefix && name) {
        m_entry = {*prefix, *name};
    }
}

std::vector<u32> LegendsDreamcastSpeech::runestoneCue(u32 count) {
    if (count == 0) {
        return {0x10079};
    }
    if (count == 1) {
        return {0x10068};
    }
    if (count > 12) {
        return {0x10077};
    }
    return {0x1006aU + count, 0x10069};
}

bool LegendsDreamcastSpeech::enqueue(SoundPlayer& output, std::span<const u32> tokens) {
    std::erase_if(m_handles, [&](SoundHandle handle) { return !output.isPlaying(handle); });
    if (m_handles.size() >= 32) {
        return false;
    }
    const auto sequence = m_sounds.phrase(tokens);
    if (!sequence) {
        return false;
    }
    // Native announcer gain224/255, track127, priority2. These ordinary entry
    // and rune phrases share one FIFO; each finite phrase owns all its clips.
    const auto handle = output.playAfter(m_handles.empty() ? kNoSound : m_handles.back(), *sequence,
                                         224.0f / 255.0f, SoundCategory::Effects);
    if (handle == kNoSound) {
        return false;
    }
    m_handles.push_back(handle);
    return true;
}

bool LegendsDreamcastSpeech::playing(const SoundPlayer& output) const {
    return std::ranges::any_of(m_handles,
                               [&](SoundHandle handle) { return output.isPlaying(handle); });
}

void LegendsDreamcastSpeech::stop(SoundPlayer& output) {
    for (const auto handle : m_handles) {
        output.stop(handle);
    }
    m_handles.clear();
}

bool LegendsDreamcastSpeech::continueFrom(SoundPlayer& output, LegendsDreamcastSpeech& previous) {
    if (this == &previous || m_root != previous.m_root || !previous.playing(output)) {
        return false;
    }
    stop(output);
    m_sounds = std::move(previous.m_sounds);
    m_handles = std::exchange(previous.m_handles, {});
    return true;
}

} // namespace gdl::game
