#pragma once

#include <filesystem>
#include <memory>
#include <span>

#include "engine/audio/StreamSource.h"

namespace gdl {

/** Verified native US MusyX effects bank. Sources share immutable native bytes,
 * but own their voices, clocks and mixer state. Pump on the calling audio-feed
 * thread, just like N64LegendsStream. No exported PCM or substitute bank. */
class N64LegendsEffects {
public:
    N64LegendsEffects();
    ~N64LegendsEffects();
    N64LegendsEffects(const N64LegendsEffects&) = delete;
    N64LegendsEffects& operator=(const N64LegendsEffects&) = delete;

    bool open(const std::filesystem::path& root);
    bool opened() const;
    bool contains(u16 cue) const;
    std::span<const u16> cueIds() const;
    /** Native numeric SFX ID, after game-side alias resolution. Null on invalid
     * IDs/settings or the bounded 64-source limit. Native loops remain loops;
     * stop their SoundPlayer handle when the owning gameplay object stops.
     * Returns native float PCM without clipping; LegendsSounds applies the
     * common N64 mix calibration before the final user-volume/limiter stages. */
    std::shared_ptr<StreamSource> create(u16 cue, f32 pitch = 1, f32 volume = 1, f32 pan = 0);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace gdl
