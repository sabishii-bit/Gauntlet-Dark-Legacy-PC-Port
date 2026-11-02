#pragma once

#include <filesystem>
#include <memory>

#include "engine/audio/StreamSource.h"

namespace gdl {

/** Native Legends MusyX music, sequenced on demand without intermediate WAV/MIDI files.
 * Only the audited US bank and song bytes are accepted by the third-party sequencer.
 * Returns neutral synth PCM; gameplay passes this through legends_audio_mix::prepare
 * before playback. Float peaks retain headroom for the final AudioMixer limiter. */
class N64LegendsStream final : public StreamSource {
public:
    N64LegendsStream();
    ~N64LegendsStream() override;
    N64LegendsStream(const N64LegendsStream&) = delete;
    N64LegendsStream& operator=(const N64LegendsStream&) = delete;

    bool open(const std::filesystem::path& root, u16 songId);
    bool opened() const;
    AudioStreamDesc desc() const override;
    bool read(std::vector<f32>& out, usize frames) override;
    void rewind() override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace gdl
