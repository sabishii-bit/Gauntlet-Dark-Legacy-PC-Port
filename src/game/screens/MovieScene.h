#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "engine/audio/AudioMixer.h"
#include "engine/audio/AudioStream.h"
#include "engine/codec/MoviePlayback.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/ImmediateBatch.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"

namespace gdl::game {

/** Plays one full-motion-video file across the whole frame with its audio track. */
class MovieScene {
public:
    /** Opens the movie and creates its GPU and audio resources; false if it cannot be played. */
    bool open(RenderDevice& device, AudioMixer& mixer, const std::filesystem::path& file);
    void close();
    bool isOpen() const { return m_playback.isOpen(); }

    /** Advances playback; returns false when the movie has finished. */
    bool update(f64 deltaSeconds);

    /** Draws the current frame stretched over `frame`. Must be the frame's first draw. */
    void render(RenderDevice& device, const Mat4& projection, const Rect& frame);

private:
    MoviePlayback m_playback;
    std::unique_ptr<Texture> m_texture;
    AudioMixer* m_mixer = nullptr;
    std::shared_ptr<AudioStream> m_audio;
    std::vector<f32> m_audioScratch;
    ImmediateBatch m_batch;
    bool m_textureDirty = false;
};

} // namespace gdl::game
