#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/codec/VqVideoDecoder.h"
#include "engine/core/Error.h"
#include "engine/math/Math.h"

#include "TestSupport.h"

namespace {

using namespace gdl;
using test::ByteWriter;

/** A codebook entry whose four pixels share one YCbCr value. */
void putEntry(ByteWriter& w, u8 luma, u8 cb, u8 cr) {
    for (int i = 0; i < 4; ++i) {
        w.putU8(luma).putU8(cb).putU8(cr);
    }
}

std::vector<u8> stored(std::span<const u8> body) {
    ByteWriter w;
    w.putU32(1).putBytes(body);
    return w.bytes();
}

/** Keyframe for a 4x4 image: `count` grey-ramp entries and one index per 2x2 block. */
std::vector<u8> keyframe(u32 count, std::span<const u32> indices) {
    ByteWriter w;
    w.putU16(static_cast<u16>(count)).putU16(1);
    for (u32 i = 0; i < count; ++i) {
        putEntry(w, static_cast<u8>(i), 128, 128);
    }
    if (count > 256) {
        u8 bits = 0;
        for (usize i = 0; i < indices.size(); ++i) {
            bits |= static_cast<u8>(((indices[i] >> 8U) & 1U) << i);
        }
        w.putU8(bits);
    }
    for (const u32 index : indices) {
        w.putU8(static_cast<u8>(index & 0xFFU));
    }
    return stored(w.bytes());
}

TEST_CASE("neutral chroma decodes to grey", "[codec][vq]") {
    VqVideoDecoder decoder(4, 4);
    const std::array<u32, 4> kIndices{128, 128, 128, 128};
    decoder.decode(keyframe(129, kIndices));
    REQUIRE(decoder.decodedFrames() == 1);
    for (u32 y = 0; y < 4; ++y) {
        for (u32 x = 0; x < 4; ++x) {
            REQUIRE(decoder.frame().pixel(x, y) == Color::rgba(128, 128, 128));
        }
    }
}

TEST_CASE("keyframe blocks are laid out bottom-up, two pixels per entry row", "[codec][vq]") {
    VqVideoDecoder decoder(4, 4);
    const std::array<u32, 4> kIndices{10, 20, 30, 40};
    decoder.decode(keyframe(41, kIndices));
    const Image& frame = decoder.frame();
    REQUIRE(frame.pixel(0, 3) == Color::rgba(10, 10, 10));
    REQUIRE(frame.pixel(1, 2) == Color::rgba(10, 10, 10));
    REQUIRE(frame.pixel(2, 3) == Color::rgba(20, 20, 20));
    REQUIRE(frame.pixel(0, 1) == Color::rgba(30, 30, 30));
    REQUIRE(frame.pixel(3, 0) == Color::rgba(40, 40, 40));
}

TEST_CASE("chroma converts with BT.601 weights and clamps", "[codec][vq]") {
    VqVideoDecoder decoder(4, 4);
    ByteWriter w;
    w.putU16(1).putU16(1);
    putEntry(w, 100, 100, 200);
    w.putU8(0).putU8(0).putU8(0).putU8(0);
    decoder.decode(stored(w.bytes()));
    // R = 100 + 1.402 * 72, G = 100 - 0.71414 * 72 - 0.34414 * -28, B = 100 + 1.772 * -28
    REQUIRE(decoder.frame().pixel(0, 0) == Color::rgba(201, 58, 50));

    ByteWriter bright;
    bright.putU16(1).putU16(1);
    putEntry(bright, 250, 128, 255);
    bright.putU8(0).putU8(0).putU8(0).putU8(0);
    decoder.decode(stored(bright.bytes()));
    REQUIRE(decoder.frame().pixel(0, 0).r == 255);
}

TEST_CASE("delta frames update only the masked cells", "[codec][vq]") {
    VqVideoDecoder decoder(8, 4);
    std::vector<u32> base(8, 0);
    decoder.decode(keyframe(1, base));
    REQUIRE(decoder.frame().pixel(0, 0) == Color::rgba(0, 0, 0));

    ByteWriter w;
    w.putU16(2).putU16(0).putU32(4);
    w.putU8(0b10); // 8x4 image = two 4x4 cells in one mask byte; only the right cell changes
    putEntry(w, 50, 128, 128);
    putEntry(w, 200, 128, 128);
    w.putU8(1).putU8(1).putU8(0).putU8(0); // lower block row then upper block row
    decoder.decode(stored(w.bytes()));

    const Image& frame = decoder.frame();
    REQUIRE(frame.pixel(0, 0) == Color::rgba(0, 0, 0));
    REQUIRE(frame.pixel(3, 3) == Color::rgba(0, 0, 0));
    REQUIRE(frame.pixel(4, 3) == Color::rgba(200, 200, 200));
    REQUIRE(frame.pixel(7, 2) == Color::rgba(200, 200, 200));
    REQUIRE(frame.pixel(4, 1) == Color::rgba(50, 50, 50));
    REQUIRE(frame.pixel(7, 0) == Color::rgba(50, 50, 50));
}

TEST_CASE("codebooks beyond 256 entries use the ninth-bit stream", "[codec][vq]") {
    VqVideoDecoder decoder(4, 4);
    const std::array<u32, 4> kIndices{300, 5, 256, 511};
    decoder.decode(keyframe(512, kIndices));
    const Image& frame = decoder.frame();
    REQUIRE(frame.pixel(0, 3) == Color::rgba(44, 44, 44)); // 300 & 0xFF
    REQUIRE(frame.pixel(2, 3) == Color::rgba(5, 5, 5));
    REQUIRE(frame.pixel(0, 1) == Color::rgba(0, 0, 0)); // 256 & 0xFF
    REQUIRE(frame.pixel(2, 1) == Color::rgba(255, 255, 255));
}

TEST_CASE("a length prefix before the packed block is honoured", "[codec][vq]") {
    const std::array<u32, 4> kIndices{7, 7, 7, 7};
    const std::vector<u8> block = keyframe(8, kIndices);
    ByteWriter w;
    w.putU32(static_cast<u32>(block.size())).putU32(0).putBytes(block).putU8(0xEE);
    VqVideoDecoder decoder(4, 4);
    decoder.decode(w.bytes());
    REQUIRE(decoder.frame().pixel(1, 1) == Color::rgba(7, 7, 7));
    REQUIRE(VqVideoDecoder::chunkPayload(w.bytes()).size() == block.size());
}

TEST_CASE("malformed frames throw FormatError", "[codec][vq]") {
    VqVideoDecoder decoder(4, 4);
    REQUIRE_THROWS_AS(VqVideoDecoder(3, 4), FormatError);
    const std::array<u32, 4> kIndices{9, 0, 0, 0};
    REQUIRE_THROWS_AS(decoder.decode(keyframe(2, kIndices)), FormatError);
    const std::array<u32, 2> kShort{0, 0};
    REQUIRE_THROWS_AS(decoder.decode(keyframe(1, kShort)), FormatError);
    REQUIRE_THROWS_AS(decoder.decode(std::vector<u8>{1, 0, 0, 0, 1}), FormatError);
}

} // namespace
