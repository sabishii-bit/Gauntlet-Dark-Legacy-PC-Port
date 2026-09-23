#include "engine/codec/VqVideoDecoder.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "engine/codec/Lzss.h"
#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

namespace gdl {

namespace {

constexpr std::size_t kBytesPerEntry = 12; ///< four YCbCr triples
constexpr std::size_t kPixelsPerEntry = 4;
constexpr std::uint32_t kNineBitThreshold = 256;
constexpr std::size_t kLengthPrefixSize = 8;

/** Reads codebook indices: one byte each, plus a leading bit stream of ninth bits when needed. */
class IndexReader {
public:
    IndexReader(std::span<const std::uint8_t> low, std::span<const std::uint8_t> high, bool nineBit)
        : m_low(low), m_high(high), m_nineBit(nineBit) {}

    std::uint32_t next() {
        if (m_lowPosition >= m_low.size()) {
            throw FormatError("VQ index stream is truncated");
        }
        std::uint32_t value = m_low[m_lowPosition++];
        if (m_nineBit) {
            if (m_highPosition >= m_high.size()) {
                throw FormatError("VQ ninth-bit stream is truncated");
            }
            value |= ((std::uint32_t{m_high[m_highPosition]} >> m_bit) & 1U) << 8U;
            if (++m_bit == 8) {
                m_bit = 0;
                ++m_highPosition;
            }
        }
        return value;
    }

private:
    std::span<const std::uint8_t> m_low;
    std::span<const std::uint8_t> m_high;
    bool m_nineBit;
    std::size_t m_lowPosition = 0;
    std::size_t m_highPosition = 0;
    std::uint32_t m_bit = 0;
};

std::uint8_t clampToByte(float value) {
    return static_cast<std::uint8_t>(std::clamp(static_cast<int>(value), 0, 255));
}

} // namespace

VqVideoDecoder::VqVideoDecoder(std::uint32_t width, std::uint32_t height)
    : m_width(width), m_height(height) {
    if (width == 0 || height == 0 || width % 4 != 0 || height % 4 != 0) {
        throw FormatError("VQ video dimensions must be non-zero multiples of four");
    }
    m_frame = Image::filled(width, height, Color::black());
}

std::span<const std::uint8_t> VqVideoDecoder::chunkPayload(std::span<const std::uint8_t> chunk) {
    if (chunk.size() >= 4 && readU32LE(chunk, 0) > 1) {
        const std::size_t length = readU32LE(chunk, 0);
        if (kLengthPrefixSize + length > chunk.size()) {
            throw FormatError("VQ chunk length prefix exceeds the chunk");
        }
        return chunk.subspan(kLengthPrefixSize, length);
    }
    return chunk;
}

void VqVideoDecoder::decode(std::span<const std::uint8_t> packedChunk) {
    const std::vector<std::uint8_t> unpacked = lzssUnpack(chunkPayload(packedChunk));
    decodeUnpacked(unpacked);
    ++m_decodedFrames;
}

void VqVideoDecoder::decodeUnpacked(std::span<const std::uint8_t> data) {
    if (data.size() < 4) {
        throw FormatError("VQ frame header is truncated");
    }
    const std::uint32_t count = readU16LE(data, 0);
    const bool keyframe = readU16LE(data, 2) != 0;
    const bool nineBit = count > kNineBitThreshold;
    const std::uint32_t blocksPerRow = m_width / 2;
    const std::uint32_t blockRows = m_height / 2;

    if (keyframe) {
        const std::size_t codebookSize = std::size_t{count} * kBytesPerEntry;
        const std::size_t indexCount = std::size_t{blocksPerRow} * blockRows;
        const std::size_t highBytes = nineBit ? (indexCount + 7) / 8 : 0;
        if (data.size() < 4 + codebookSize + highBytes + indexCount) {
            throw FormatError("VQ keyframe is truncated");
        }
        buildCodebook(data.subspan(4, codebookSize), count);
        IndexReader indices(data.subspan(4 + codebookSize + highBytes, indexCount),
                            data.subspan(4 + codebookSize, highBytes), nineBit);
        for (std::uint32_t row = 0; row < blockRows; ++row) {
            for (std::uint32_t x = 0; x < blocksPerRow; ++x) {
                writeBlock(x, row, indices.next());
            }
        }
        return;
    }

    const std::uint32_t maskStride = (m_width + 31) / 32;
    const std::size_t maskBytes = std::size_t{maskStride} * (m_height / 4);
    const std::size_t codebookSize = std::size_t{count} * kBytesPerEntry;
    if (data.size() < 8 + maskBytes + codebookSize) {
        throw FormatError("VQ delta frame is truncated");
    }
    const std::size_t indexCount = readU32LE(data, 4);
    const std::size_t highBytes = nineBit ? (indexCount + 7) / 8 : 0;
    const std::span<const std::uint8_t> mask = data.subspan(8, maskBytes);
    buildCodebook(data.subspan(8 + maskBytes, codebookSize), count);
    const std::size_t streamStart = 8 + maskBytes + codebookSize;
    if (data.size() < streamStart + highBytes) {
        throw FormatError("VQ delta frame index streams are truncated");
    }
    IndexReader indices(data.subspan(streamStart + highBytes), data.subspan(streamStart, highBytes),
                        nineBit);
    for (std::uint32_t row = 0; row < blockRows; ++row) {
        const std::size_t maskRow = std::size_t{row / 2} * maskStride;
        for (std::uint32_t x = 0; x < blocksPerRow; ++x) {
            const std::uint32_t cell = x / 2;
            if (((mask[maskRow + (cell / 8)] >> (cell % 8)) & 1U) != 0) {
                writeBlock(x, row, indices.next());
            }
        }
    }
}

void VqVideoDecoder::buildCodebook(std::span<const std::uint8_t> entries, std::uint32_t count) {
    constexpr float kChromaOffset = 128.0f;
    constexpr float kRedFromCr = 1.402f;
    constexpr float kGreenFromCb = 0.34414f;
    constexpr float kGreenFromCr = 0.71414f;
    constexpr float kBlueFromCb = 1.772f;
    constexpr float kRounding = 0.5f;

    m_codebook.resize(std::size_t{count} * kPixelsPerEntry * 3);
    for (std::size_t i = 0; i < std::size_t{count} * kPixelsPerEntry; ++i) {
        const std::size_t in = i * 3;
        const float luma = entries[in];
        const float cb = static_cast<float>(entries[in + 1]) - kChromaOffset;
        const float cr = static_cast<float>(entries[in + 2]) - kChromaOffset;
        m_codebook[in] = clampToByte(kRounding + luma + kRedFromCr * cr);
        m_codebook[in + 1] = clampToByte(kRounding + luma - kGreenFromCr * cr - kGreenFromCb * cb);
        m_codebook[in + 2] = clampToByte(kRounding + luma + kBlueFromCb * cb);
    }
}

void VqVideoDecoder::writeBlock(std::uint32_t blockX, std::uint32_t blockRow, std::uint32_t index) {
    const std::size_t entry = std::size_t{index} * kPixelsPerEntry * 3;
    if (entry + kPixelsPerEntry * 3 > m_codebook.size()) {
        throw FormatError("VQ index outside the codebook");
    }
    const std::uint32_t x = blockX * 2;
    const std::uint32_t lowerRow = m_height - 1 - blockRow * 2;
    const std::uint32_t upperRow = lowerRow - 1;
    const std::span<const std::uint8_t> rgb =
        std::span(m_codebook).subspan(entry, kPixelsPerEntry * 3);
    m_frame.setPixel(x, lowerRow, Color::rgba(rgb[0], rgb[1], rgb[2]));
    m_frame.setPixel(x + 1, lowerRow, Color::rgba(rgb[3], rgb[4], rgb[5]));
    m_frame.setPixel(x, upperRow, Color::rgba(rgb[6], rgb[7], rgb[8]));
    m_frame.setPixel(x + 1, upperRow, Color::rgba(rgb[9], rgb[10], rgb[11]));
}

} // namespace gdl
