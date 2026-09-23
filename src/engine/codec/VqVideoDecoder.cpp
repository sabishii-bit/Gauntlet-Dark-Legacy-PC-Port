#include "engine/codec/VqVideoDecoder.h"

#include <algorithm>

#include "engine/codec/Lzss.h"
#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/ByteReader.h"

namespace gdl {

namespace {

constexpr usize kBytesPerEntry = 12; ///< four YCbCr triples
constexpr usize kPixelsPerEntry = 4;
constexpr u32 kNineBitThreshold = 256;
constexpr usize kLengthPrefixSize = 8;

/** Reads codebook indices: one byte each, plus a leading bit stream of ninth bits when needed. */
class IndexReader {
public:
    IndexReader(std::span<const u8> low, std::span<const u8> high, bool nineBit)
        : m_low(low), m_high(high), m_nineBit(nineBit) {}

    u32 next() {
        if (m_lowPosition >= m_low.size()) {
            throw FormatError("VQ index stream is truncated");
        }
        u32 value = m_low[m_lowPosition++];
        if (m_nineBit) {
            if (m_highPosition >= m_high.size()) {
                throw FormatError("VQ ninth-bit stream is truncated");
            }
            value |= ((u32{m_high[m_highPosition]} >> m_bit) & 1U) << 8U;
            if (++m_bit == 8) {
                m_bit = 0;
                ++m_highPosition;
            }
        }
        return value;
    }

private:
    std::span<const u8> m_low;
    std::span<const u8> m_high;
    bool m_nineBit;
    usize m_lowPosition = 0;
    usize m_highPosition = 0;
    u32 m_bit = 0;
};

u8 clampToByte(f32 value) {
    return static_cast<u8>(std::clamp(static_cast<s32>(value), 0, 255));
}

} // namespace

VqVideoDecoder::VqVideoDecoder(u32 width, u32 height) : m_width(width), m_height(height) {
    if (width == 0 || height == 0 || width % 4 != 0 || height % 4 != 0) {
        throw FormatError("VQ video dimensions must be non-zero multiples of four");
    }
    m_frame = Image::filled(width, height, Color::black());
}

std::span<const u8> VqVideoDecoder::chunkPayload(std::span<const u8> chunk) {
    if (chunk.size() >= 4 && readU32LE(chunk, 0) > 1) {
        const usize length = readU32LE(chunk, 0);
        if (kLengthPrefixSize + length > chunk.size()) {
            throw FormatError("VQ chunk length prefix exceeds the chunk");
        }
        return chunk.subspan(kLengthPrefixSize, length);
    }
    return chunk;
}

void VqVideoDecoder::decode(std::span<const u8> packedChunk) {
    const std::vector<u8> unpacked = lzssUnpack(chunkPayload(packedChunk));
    decodeUnpacked(unpacked);
    ++m_decodedFrames;
}

void VqVideoDecoder::decodeUnpacked(std::span<const u8> data) {
    if (data.size() < 4) {
        throw FormatError("VQ frame header is truncated");
    }
    const u32 count = readU16LE(data, 0);
    const bool keyframe = readU16LE(data, 2) != 0;
    const bool nineBit = count > kNineBitThreshold;
    const u32 blocksPerRow = m_width / 2;
    const u32 blockRows = m_height / 2;

    if (keyframe) {
        const usize codebookSize = usize{count} * kBytesPerEntry;
        const usize indexCount = usize{blocksPerRow} * blockRows;
        const usize highBytes = nineBit ? (indexCount + 7) / 8 : 0;
        if (data.size() < 4 + codebookSize + highBytes + indexCount) {
            throw FormatError("VQ keyframe is truncated");
        }
        buildCodebook(data.subspan(4, codebookSize), count);
        IndexReader indices(data.subspan(4 + codebookSize + highBytes, indexCount),
                            data.subspan(4 + codebookSize, highBytes), nineBit);
        for (u32 row = 0; row < blockRows; ++row) {
            for (u32 x = 0; x < blocksPerRow; ++x) {
                writeBlock(x, row, indices.next());
            }
        }
        return;
    }

    const u32 maskStride = (m_width + 31) / 32;
    const usize maskBytes = usize{maskStride} * (m_height / 4);
    const usize codebookSize = usize{count} * kBytesPerEntry;
    if (data.size() < 8 + maskBytes + codebookSize) {
        throw FormatError("VQ delta frame is truncated");
    }
    const usize indexCount = readU32LE(data, 4);
    const usize highBytes = nineBit ? (indexCount + 7) / 8 : 0;
    const std::span<const u8> mask = data.subspan(8, maskBytes);
    buildCodebook(data.subspan(8 + maskBytes, codebookSize), count);
    const usize streamStart = 8 + maskBytes + codebookSize;
    if (data.size() < streamStart + highBytes) {
        throw FormatError("VQ delta frame index streams are truncated");
    }
    IndexReader indices(data.subspan(streamStart + highBytes), data.subspan(streamStart, highBytes),
                        nineBit);
    for (u32 row = 0; row < blockRows; ++row) {
        const usize maskRow = usize{row / 2} * maskStride;
        for (u32 x = 0; x < blocksPerRow; ++x) {
            const u32 cell = x / 2;
            if (((mask[maskRow + (cell / 8)] >> (cell % 8)) & 1U) != 0) {
                writeBlock(x, row, indices.next());
            }
        }
    }
}

void VqVideoDecoder::buildCodebook(std::span<const u8> entries, u32 count) {
    constexpr f32 kChromaOffset = 128.0f;
    constexpr f32 kRedFromCr = 1.402f;
    constexpr f32 kGreenFromCb = 0.34414f;
    constexpr f32 kGreenFromCr = 0.71414f;
    constexpr f32 kBlueFromCb = 1.772f;
    constexpr f32 kRounding = 0.5f;

    m_codebook.resize(usize{count} * kPixelsPerEntry * 3);
    for (usize i = 0; i < usize{count} * kPixelsPerEntry; ++i) {
        const usize in = i * 3;
        const f32 luma = entries[in];
        const f32 cb = static_cast<f32>(entries[in + 1]) - kChromaOffset;
        const f32 cr = static_cast<f32>(entries[in + 2]) - kChromaOffset;
        m_codebook[in] = clampToByte(kRounding + luma + kRedFromCr * cr);
        m_codebook[in + 1] = clampToByte(kRounding + luma - kGreenFromCr * cr - kGreenFromCb * cb);
        m_codebook[in + 2] = clampToByte(kRounding + luma + kBlueFromCb * cb);
    }
}

void VqVideoDecoder::writeBlock(u32 blockX, u32 blockRow, u32 index) {
    const usize entry = usize{index} * kPixelsPerEntry * 3;
    if (entry + kPixelsPerEntry * 3 > m_codebook.size()) {
        throw FormatError("VQ index outside the codebook");
    }
    const u32 x = blockX * 2;
    const u32 lowerRow = m_height - 1 - blockRow * 2;
    const u32 upperRow = lowerRow - 1;
    const std::span<const u8> rgb = std::span(m_codebook).subspan(entry, kPixelsPerEntry * 3);
    m_frame.setPixel(x, lowerRow, Color::rgba(rgb[0], rgb[1], rgb[2]));
    m_frame.setPixel(x + 1, lowerRow, Color::rgba(rgb[3], rgb[4], rgb[5]));
    m_frame.setPixel(x, upperRow, Color::rgba(rgb[6], rgb[7], rgb[8]));
    m_frame.setPixel(x + 1, upperRow, Color::rgba(rgb[9], rgb[10], rgb[11]));
}

} // namespace gdl
