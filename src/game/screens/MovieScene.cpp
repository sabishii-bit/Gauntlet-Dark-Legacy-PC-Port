#include "game/screens/MovieScene.h"

namespace gdl::game {

namespace {

constexpr f32 kMovieDepth = 0.5f;

} // namespace

bool MovieScene::open(RenderDevice& device, AudioMixer& mixer, const std::filesystem::path& file) {
    close();
    if (!m_playback.open(file)) {
        return false;
    }
    const MovieInfo& info = m_playback.info();
    m_texture = device.createTexture(TextureDesc{info.width, info.height, TextureFilter::Linear},
                                     m_playback.frame().pixels);
    m_mixer = &mixer;
    m_textureDirty = false;
    return true;
}

void MovieScene::close() {
    if (m_audio) {
        m_audio->stop();
        m_audio.reset();
    }
    m_mixer = nullptr;
    m_texture.reset();
    m_playback.close();
    m_audioScratch.clear();
    m_textureDirty = false;
}

bool MovieScene::update(f64 deltaSeconds) {
    if (!isOpen()) {
        return false;
    }
    const bool playing = m_playback.update(deltaSeconds);
    if (m_playback.frameChanged()) {
        m_textureDirty = true;
    }

    const MovieInfo& info = m_playback.info();
    if (!m_audio && info.hasAudio && info.audioReady && m_mixer != nullptr) {
        m_audio = m_mixer->createStream(AudioStreamDesc{info.audioSampleRate, info.audioChannels});
    }
    if (m_audio) {
        m_audioScratch.clear();
        m_playback.takeAudio(m_audioScratch);
        if (!m_audioScratch.empty()) {
            m_audio->push(m_audioScratch);
        }
        if (!playing) {
            m_audio->finish();
        }
    }
    return playing;
}

void MovieScene::render(RenderDevice& device, const Mat4& projection, const Rect& frame) {
    if (!m_texture) {
        return;
    }
    if (m_textureDirty) {
        device.updateTexture(*m_texture, m_playback.frame().pixels);
        m_textureDirty = false;
    }
    m_batch.clear();
    m_batch.rect(frame, kMovieDepth, Color::white());
    device.draw(m_batch, *m_texture, projection);
}

} // namespace gdl::game
