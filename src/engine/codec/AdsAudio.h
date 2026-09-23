#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "engine/codec/DspAdpcm.h"

namespace gdl {

struct AdsAudioInfo {
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    std::uint32_t blockSize = 0;   ///< bytes per channel between interleave switches
    std::uint32_t bodySize = 0;    ///< bytes of ADPCM data following the headers
    std::uint32_t sampleCount = 0; ///< per channel
};

/**
 * Decoder for the "SShd"/"SSbd" audio streams the game ships in its STREAMS folder and embeds in
 * its movies: a small header, one DSP-ADPCM coefficient block per channel, then ADPCM frames
 * interleaved per channel in blocks of `blockSize` bytes. Data can be fed in any size pieces.
 */
class AdsAudioDecoder {
public:
    /** True when `bytes` begins with an ADS header tag. */
    static bool looksLikeAds(std::span<const std::uint8_t> bytes);

    /** Bytes the header occupies for a stream with this many channels. */
    static std::size_t headerSize(std::uint32_t channels);

    /** Consumes the header; returns the number of bytes used or nothing if more are needed. */
    std::optional<std::size_t> parseHeader(std::span<const std::uint8_t> bytes);

    bool hasHeader() const { return m_info.channels != 0; }
    const AdsAudioInfo& info() const { return m_info; }

    /** Decodes as much of `bytes` as forms complete interleave rounds; the rest is kept. */
    void feed(std::span<const std::uint8_t> bytes, std::vector<float>& interleavedOut);

    /** Decodes whatever partial round is still buffered (end of stream). */
    void flush(std::vector<float>& interleavedOut);

private:
    void decodeRound(std::span<const std::uint8_t> round, std::vector<float>& interleavedOut);

    AdsAudioInfo m_info;
    std::vector<DspAdpcmDecoder> m_channels;
    std::vector<std::uint8_t> m_pending;
    std::vector<std::int16_t> m_scratch;
};

} // namespace gdl
