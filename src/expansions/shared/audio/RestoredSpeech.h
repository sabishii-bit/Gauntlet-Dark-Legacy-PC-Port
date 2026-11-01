#pragma once

#include <memory>
#include <vector>

#include "engine/audio/MusicDeclicker.h"
#include "engine/audio/StreamSource.h"

namespace gdl {

/** Optional expansion speech restoration, never applied to ordinary GameCube audio.
 * Owns the clips' playback cursors. Up to 64 finite mono/stereo clips must share a
 * 1000..192000 Hz format. Invalid input exposes {0,0}; rates below 8000 bypass
 * restoration. Each clip is independently flushed, so phrase boundaries retain
 * exactly their authored duration and never share prediction context. */
class RestoredSpeech final : public StreamSource {
public:
    explicit RestoredSpeech(std::vector<std::shared_ptr<StreamSource>> clips);

    AudioStreamDesc desc() const override { return m_desc; }
    bool read(std::vector<f32>& output, usize frames) override;
    void rewind() override;

private:
    bool beginClip();
    bool pump();

    std::vector<std::shared_ptr<StreamSource>> m_clips;
    AudioStreamDesc m_desc{0, 0};
    MusicDeclicker m_impulses;
    MusicDeclicker m_bursts;
    std::vector<f32> m_input;
    std::vector<f32> m_intermediate;
    std::vector<f32> m_ready;
    usize m_clip = 0;
    usize m_cursor = 0;
    bool m_finishedClip = false;
    bool m_failed = false;
};

} // namespace gdl
