#pragma once

#include <optional>
#include <span>
#include <vector>

#include "engine/codec/DspAdpcm.h"
#include "engine/core/Types.h"

namespace gdl {

struct AdsAudioInfo {
    u32 sampleRate = 0;
    u32 channels = 0;
    u32 blockSize = 0;   ///< bytes per channel between interleave switches
    u32 bodySize = 0;    ///< bytes of ADPCM data following the headers
    u32 sampleCount = 0; ///< per channel
};

/**
 * Decoder for the "SShd"/"SSbd" audio streams the game ships in its STREAMS folder and embeds in
 * its movies: a small header, one DSP-ADPCM coefficient block per channel, then ADPCM frames
 * interleaved per channel in blocks of `blockSize` bytes. Data can be fed in any size pieces.
 */
class AdsAudioDecoder {
public:
    /** True when `bytes` begins with an ADS header tag. */
    static bool looksLikeAds(std::span<const u8> bytes);

    /** Bytes the header occupies for a stream with this many channels. */
    static usize headerSize(u32 channels);

    /** Consumes the header; returns the number of bytes used or nothing if more are needed. */
    std::optional<usize> parseHeader(std::span<const u8> bytes);

    bool hasHeader() const { return m_info.channels != 0; }
    const AdsAudioInfo& info() const { return m_info; }

    /** Decodes as much of `bytes` as forms complete interleave rounds; the rest is kept. */
    void feed(std::span<const u8> bytes, std::vector<f32>& interleavedOut);

    /** Decodes whatever partial round is still buffered (end of stream). */
    void flush(std::vector<f32>& interleavedOut);

private:
    void decodeRound(std::span<const u8> round, std::vector<f32>& interleavedOut);

    AdsAudioInfo m_info;
    std::vector<DspAdpcmDecoder> m_channels;
    std::vector<u8> m_pending;
    std::vector<s16> m_scratch;
};

} // namespace gdl
