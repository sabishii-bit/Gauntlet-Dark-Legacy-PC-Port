#pragma once

#include <span>
#include <vector>

#include "engine/core/Types.h"
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
    VqVideoDecoder(u32 width, u32 height);

    /** Decodes one packed frame chunk into frame(); throws FormatError on malformed data. */
    void decode(std::span<const u8> packedChunk);

    const Image& frame() const { return m_frame; }
    u32 decodedFrames() const { return m_decodedFrames; }

    /** Strips the optional eight-byte length prefix some chunks carry before the LZSS block. */
    static std::span<const u8> chunkPayload(std::span<const u8> chunk);

private:
    void decodeUnpacked(std::span<const u8> data);
    void buildCodebook(std::span<const u8> entries, u32 count);
    void writeBlock(u32 blockX, u32 blockRow, u32 index);

    u32 m_width;
    u32 m_height;
    Image m_frame;
    std::vector<u8> m_codebook; ///< count * 4 pixels * RGB
    u32 m_decodedFrames = 0;
};

} // namespace gdl
