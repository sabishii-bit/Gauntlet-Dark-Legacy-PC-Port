#include "engine/codec/AdsAudio.h"

#include <string_view>

#include "engine/core/Error.h"

namespace gdl {

namespace {

constexpr usize kHeaderChunkSize = 32;
constexpr usize kBodyChunkSize = 8;
constexpr usize kChannelHeaderSize = 96;
constexpr usize kCoefficientOffset = 0x1C;
constexpr u32 kFormatAdpcm = 32;
constexpr f32 kSampleScale = 1.0f / 32768.0f;

constexpr u32 readBigU32(std::span<const u8> bytes, usize offset) {
    return (u32{bytes[offset]} << 24U) | (u32{bytes[offset + 1]} << 16U) |
           (u32{bytes[offset + 2]} << 8U) | u32{bytes[offset + 3]};
}

bool isTag(std::span<const u8> bytes, usize offset, std::string_view tag) {
    for (usize i = 0; i < tag.size(); ++i) {
        if (bytes[offset + i] != static_cast<u8>(tag[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

bool AdsAudioDecoder::looksLikeAds(std::span<const u8> bytes) {
    return bytes.size() >= 4 && (isTag(bytes, 0, "dhSS") || isTag(bytes, 0, "SShd"));
}

usize AdsAudioDecoder::headerSize(u32 channels) {
    return kHeaderChunkSize + kBodyChunkSize + kChannelHeaderSize * channels;
}

std::optional<usize> AdsAudioDecoder::parseHeader(std::span<const u8> bytes) {
    if (bytes.size() < kHeaderChunkSize + kBodyChunkSize) {
        return std::nullopt;
    }
    if (!looksLikeAds(bytes)) {
        throw FormatError("not an ADS audio stream");
    }
    const u32 format = readBigU32(bytes, 0x08);
    const u32 sampleRate = readBigU32(bytes, 0x0C);
    const u32 channels = readBigU32(bytes, 0x10);
    const u32 blockSize = readBigU32(bytes, 0x14);
    if (!isTag(bytes, kHeaderChunkSize, "dbSS") && !isTag(bytes, kHeaderChunkSize, "SSbd")) {
        throw FormatError("ADS stream has no body chunk");
    }
    const u32 bodySize = readBigU32(bytes, kHeaderChunkSize + 4);
    if (format != kFormatAdpcm || channels == 0 || channels > 2 || sampleRate == 0 ||
        blockSize == 0 || blockSize % DspAdpcmDecoder::kFrameBytes != 0) {
        throw FormatError("unsupported ADS stream parameters");
    }
    const usize needed = headerSize(channels);
    if (bytes.size() < needed) {
        return std::nullopt;
    }

    m_channels.clear();
    for (u32 c = 0; c < channels; ++c) {
        const usize base = kHeaderChunkSize + kBodyChunkSize + kChannelHeaderSize * c;
        m_channels.emplace_back(
            readDspAdpcmCoefficients(bytes.subspan(base + kCoefficientOffset, 32)));
    }
    m_info = AdsAudioInfo{sampleRate, channels, blockSize, bodySize,
                          readBigU32(bytes, kHeaderChunkSize + kBodyChunkSize)};
    m_pending.clear();
    return needed;
}

void AdsAudioDecoder::feed(std::span<const u8> bytes, std::vector<f32>& interleavedOut) {
    if (!hasHeader()) {
        throw FormatError("ADS data fed before its header");
    }
    m_pending.insert(m_pending.end(), bytes.begin(), bytes.end());
    const usize roundSize = usize{m_info.blockSize} * m_info.channels;
    usize offset = 0;
    while (m_pending.size() - offset >= roundSize) {
        decodeRound(std::span(m_pending).subspan(offset, roundSize), interleavedOut);
        offset += roundSize;
    }
    m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(offset));
}

void AdsAudioDecoder::flush(std::vector<f32>& interleavedOut) {
    if (!hasHeader() || m_pending.empty()) {
        return;
    }
    const usize roundSize = usize{m_info.blockSize} * m_info.channels;
    m_pending.resize(roundSize, 0);
    decodeRound(m_pending, interleavedOut);
    m_pending.clear();
}

void AdsAudioDecoder::decodeRound(std::span<const u8> round, std::vector<f32>& interleavedOut) {
    const usize channels = m_info.channels;
    const usize samplesPerChannel =
        (m_info.blockSize / DspAdpcmDecoder::kFrameBytes) * DspAdpcmDecoder::kSamplesPerFrame;
    m_scratch.clear();
    for (usize c = 0; c < channels; ++c) {
        m_channels[c].decode(round.subspan(c * m_info.blockSize, m_info.blockSize), m_scratch);
    }
    interleavedOut.reserve(interleavedOut.size() + samplesPerChannel * channels);
    for (usize i = 0; i < samplesPerChannel; ++i) {
        for (usize c = 0; c < channels; ++c) {
            interleavedOut.push_back(static_cast<f32>(m_scratch[c * samplesPerChannel + i]) *
                                     kSampleScale);
        }
    }
}

} // namespace gdl
