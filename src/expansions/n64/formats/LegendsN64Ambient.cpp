#include "expansions/n64/formats/LegendsN64Ambient.h"

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {

std::vector<LegendsN64Ambient> decodeLegendsN64Ambient(std::span<const u8> placements) {
    if (placements.empty()) {
        return {};
    }
    const auto range = [&](usize at, usize count) {
        if (at > placements.size() || count > placements.size() - at) {
            throw FormatError("N64 ambient sound: truncated placement data");
        }
    };
    // e0062dfc also writes nine rebased runtime pointers at IPR+56..+88;
    // those reserved slots are part of the 92-byte header, not payload.
    range(0, 92);
    // e0062dfc rebases IPR+28 and reads the count at +52. e007e0e0
    // expands each twelve-byte authored record into a 28-byte runtime voice.
    const usize count = readU16BE(placements, 52);
    if (count == 0) {
        return {};
    }
    const usize first = readU32BE(placements, 28);
    if (first < 92 || count > 4096) {
        throw FormatError("N64 ambient sound: invalid placement range");
    }
    range(first, count * 12);
    std::vector<LegendsN64Ambient> result;
    result.reserve(count);
    for (usize i = 0; i < count; ++i) {
        const usize at = first + i * 12;
        LegendsN64Ambient sound;
        sound.nativeIndex = static_cast<u32>(i);
        for (usize axis = 0; axis < 3; ++axis) {
            sound.position[static_cast<s32>(axis)] =
                static_cast<f32>(static_cast<s16>(readU16BE(placements, at + axis * 2))) / 4;
        }
        sound.kind = placements[at + 6];
        sound.subtype = placements[at + 7];
        sound.token = readU16BE(placements, at + 8);
        // Unlike the coordinates, the signed radius is already in world units.
        sound.radius = static_cast<f32>(static_cast<s16>(readU16BE(placements, at + 10)));
        if (sound.radius <= 0) {
            throw FormatError("N64 ambient sound: nonpositive audible radius");
        }
        result.push_back(sound);
    }
    return result;
}

} // namespace gdl::formats
