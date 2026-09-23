#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "engine/render/Image.h"

namespace gdl {

/**
 * Decoder for the vector-quantised "MVDV" video stream: each frame carries a codebook of
 * 2x2 pixel blocks and either an index for every block (keyframe) or a 4x4-cell update mask
 * with indices for the changed cells only (delta frame). Frames are LZSS packed.
 */
class VqVideoDecoder {
public:
    /** Width and height must be multiples of four. */
    VqVideoDecoder(std::uint32_t width, std::uint32_t height);

    /** Decodes one packed frame chunk into frame(); throws FormatError on malformed data. */
    void decode(std::span<const std::uint8_t> packedChunk);

    const Image& frame() const { return m_frame; }
    std::uint32_t decodedFrames() const { return m_decodedFrames; }

    /** Strips the optional eight-byte length prefix some chunks carry before the LZSS block. */
    static std::span<const std::uint8_t> chunkPayload(std::span<const std::uint8_t> chunk);

private:
    void decodeUnpacked(std::span<const std::uint8_t> data);
    void buildCodebook(std::span<const std::uint8_t> entries, std::uint32_t count);
    void writeBlock(std::uint32_t blockX, std::uint32_t blockRow, std::uint32_t index);

    std::uint32_t m_width;
    std::uint32_t m_height;
    Image m_frame;
    std::vector<std::uint8_t> m_codebook; ///< count * 4 pixels * RGB
    std::uint32_t m_decodedFrames = 0;
};

} // namespace gdl
