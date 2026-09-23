#include "engine/codec/AdsAudio.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "engine/core/Error.h"

namespace gdl {

namespace {

constexpr std::size_t kHeaderChunkSize = 32;
constexpr std::size_t kBodyChunkSize = 8;
constexpr std::size_t kChannelHeaderSize = 96;
constexpr std::size_t kCoefficientOffset = 0x1C;
constexpr std::uint32_t kFormatAdpcm = 32;
constexpr float kSampleScale = 1.0f / 32768.0f;

constexpr std::uint32_t readBigU32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (std::uint32_t{bytes[offset]} << 24U) | (std::uint32_t{bytes[offset + 1]} << 16U) |
           (std::uint32_t{bytes[offset + 2]} << 8U) | std::uint32_t{bytes[offset + 3]};
}

bool isTag(std::span<const std::uint8_t> bytes, std::size_t offset, std::string_view tag) {
    for (std::size_t i = 0; i < tag.size(); ++i) {
        if (bytes[offset + i] != static_cast<std::uint8_t>(tag[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

bool AdsAudioDecoder::looksLikeAds(std::span<const std::uint8_t> bytes) {
    return bytes.size() >= 4 && (isTag(bytes, 0, "dhSS") || isTag(bytes, 0, "SShd"));
}

std::size_t AdsAudioDecoder::headerSize(std::uint32_t channels) {
    return kHeaderChunkSize + kBodyChunkSize + kChannelHeaderSize * channels;
}

std::optional<std::size_t> AdsAudioDecoder::parseHeader(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kHeaderChunkSize + kBodyChunkSize) {
        return std::nullopt;
    }
    if (!looksLikeAds(bytes)) {
        throw FormatError("not an ADS audio stream");
    }
    const std::uint32_t format = readBigU32(bytes, 0x08);
    const std::uint32_t sampleRate = readBigU32(bytes, 0x0C);
    const std::uint32_t channels = readBigU32(bytes, 0x10);
    const std::uint32_t blockSize = readBigU32(bytes, 0x14);
    if (!isTag(bytes, kHeaderChunkSize, "dbSS") && !isTag(bytes, kHeaderChunkSize, "SSbd")) {
        throw FormatError("ADS stream has no body chunk");
    }
    const std::uint32_t bodySize = readBigU32(bytes, kHeaderChunkSize + 4);
    if (format != kFormatAdpcm || channels == 0 || channels > 2 || sampleRate == 0 ||
        blockSize == 0 || blockSize % DspAdpcmDecoder::kFrameBytes != 0) {
        throw FormatError("unsupported ADS stream parameters");
    }
    const std::size_t needed = headerSize(channels);
    if (bytes.size() < needed) {
        return std::nullopt;
    }

    m_channels.clear();
    for (std::uint32_t c = 0; c < channels; ++c) {
        const std::size_t base = kHeaderChunkSize + kBodyChunkSize + kChannelHeaderSize * c;
        m_channels.emplace_back(
            readDspAdpcmCoefficients(bytes.subspan(base + kCoefficientOffset, 32)));
    }
    m_info = AdsAudioInfo{sampleRate, channels, blockSize, bodySize,
                          readBigU32(bytes, kHeaderChunkSize + kBodyChunkSize)};
    m_pending.clear();
    return needed;
}

void AdsAudioDecoder::feed(std::span<const std::uint8_t> bytes,
                           std::vector<float>& interleavedOut) {
    if (!hasHeader()) {
        throw FormatError("ADS data fed before its header");
    }
    m_pending.insert(m_pending.end(), bytes.begin(), bytes.end());
    const std::size_t roundSize = std::size_t{m_info.blockSize} * m_info.channels;
    std::size_t offset = 0;
    while (m_pending.size() - offset >= roundSize) {
        decodeRound(std::span(m_pending).subspan(offset, roundSize), interleavedOut);
        offset += roundSize;
    }
    m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(offset));
}

void AdsAudioDecoder::flush(std::vector<float>& interleavedOut) {
    if (!hasHeader() || m_pending.empty()) {
        return;
    }
    const std::size_t roundSize = std::size_t{m_info.blockSize} * m_info.channels;
    m_pending.resize(roundSize, 0);
    decodeRound(m_pending, interleavedOut);
    m_pending.clear();
}

void AdsAudioDecoder::decodeRound(std::span<const std::uint8_t> round,
                                  std::vector<float>& interleavedOut) {
    const std::size_t channels = m_info.channels;
    const std::size_t samplesPerChannel =
        (m_info.blockSize / DspAdpcmDecoder::kFrameBytes) * DspAdpcmDecoder::kSamplesPerFrame;
    m_scratch.clear();
    for (std::size_t c = 0; c < channels; ++c) {
        m_channels[c].decode(round.subspan(c * m_info.blockSize, m_info.blockSize), m_scratch);
    }
    interleavedOut.reserve(interleavedOut.size() + samplesPerChannel * channels);
    for (std::size_t i = 0; i < samplesPerChannel; ++i) {
        for (std::size_t c = 0; c < channels; ++c) {
            interleavedOut.push_back(static_cast<float>(m_scratch[c * samplesPerChannel + i]) *
                                     kSampleScale);
        }
    }
}

} // namespace gdl
