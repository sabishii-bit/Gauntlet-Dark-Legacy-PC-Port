#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "engine/audio/StreamSource.h"
#include "engine/codec/AdsAudio.h"

namespace gdl {

/**
 * One of the game's ADS audio streams (the .ads files in STREAMS, the level music) read from a
 * file: decoded a piece at a time as the mixer needs it, and rewound to play again.
 */
class AdsStream final : public StreamSource {
public:
    static constexpr std::size_t kPieceBytes = std::size_t{64} * 1024; ///< decoded per read at most

    /** Reads the file and its header; false (with a warning) when it cannot be played. */
    bool open(const std::filesystem::path& file);
    bool opened() const { return m_info.channels != 0; }
    const AdsAudioInfo& info() const { return m_info; }
    /** The stream's length in seconds. */
    double seconds() const;

    AudioStreamDesc desc() const override;
    bool read(std::vector<float>& out, std::size_t frames) override;
    void rewind() override;

private:
    std::vector<std::uint8_t> m_bytes;
    std::size_t m_dataStart = 0;
    std::size_t m_dataEnd = 0;
    std::size_t m_offset = 0;
    AdsAudioDecoder m_decoder;
    AdsAudioInfo m_info;
    bool m_flushed = false;
};

} // namespace gdl
