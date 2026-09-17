#pragma once

#include <filesystem>
#include <vector>

#include "engine/audio/StreamSource.h"
#include "engine/codec/AdsAudio.h"
#include "engine/core/Types.h"

namespace gdl {

/**
 * One of the game's ADS audio streams (`STREAMS/*.ads`, the level music) read from a file:
 * decoded a piece at a time as the mixer needs it, and rewound to play again.
 */
class AdsStream final : public StreamSource {
public:
    static constexpr usize kPieceBytes = usize{64} * 1024; ///< decoded per read at most

    /** Reads the file and its header; false (with a warning) when it cannot be played. */
    bool open(const std::filesystem::path& file);
    bool opened() const { return m_info.channels != 0; }
    const AdsAudioInfo& info() const { return m_info; }
    /** The stream's length in seconds. */
    f64 seconds() const;

    AudioStreamDesc desc() const override;
    bool read(std::vector<f32>& out, usize frames) override;
    void rewind() override;

private:
    std::vector<u8> m_bytes;
    usize m_dataStart = 0;
    usize m_dataEnd = 0;
    usize m_offset = 0;
    AdsAudioDecoder m_decoder;
    AdsAudioInfo m_info;
    bool m_flushed = false;
};

} // namespace gdl
