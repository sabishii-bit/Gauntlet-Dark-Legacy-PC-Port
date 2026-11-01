#include "expansions/n64/formats/LegendsN64Font.h"

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {

LegendsN64Font LegendsN64Font::parse(std::span<const u8> bytes) {
    constexpr usize kHeader = 0x24;
    constexpr usize kGlyph = 5;
    if (bytes.size() < kHeader || bytes[1] != 4 || bytes[2] != 0) {
        throw FormatError("N64 font: expected native I4 header");
    }
    const u32 width = readU16BE(bytes, 8);
    const u32 height = readU16BE(bytes, 10);
    const s32 lineHeight = readU16BE(bytes, 12);
    const s32 space = readU16BE(bytes, 14);
    const u32 flags = readU32BE(bytes, 16);
    const usize count = readU32BE(bytes, 20);
    if (!width || !height || width > 1024 || height > 1024 || !lineHeight || !space || count == 0 ||
        count > 256 || count > (bytes.size() - kHeader) / kGlyph) {
        throw FormatError("N64 font: invalid dimensions or glyph count");
    }
    // e0016f30 binds glyphs at +0x24 and the atlas at align16(0x24+count*5).
    const usize pixels = (kHeader + count * kGlyph + 15) & ~usize{15};
    const usize texels = usize{width} * height;
    if (pixels > bytes.size() || (texels + 1) / 2 > bytes.size() - pixels) {
        throw FormatError("N64 font: truncated atlas");
    }
    std::vector<BitmapGlyph> glyphs;
    u8 previous = 0;
    for (usize i = 0; i < count; ++i) {
        const usize at = kHeader + i * kGlyph;
        const u8 code = bytes[at];
        const s32 x = bytes[at + 1];
        const s32 y = bytes[at + 2];
        const s32 columns = bytes[at + 3];
        const s32 rows = bytes[at + 4];
        if (code <= previous || !columns || !rows || x + columns > static_cast<s32>(width) ||
            y + rows > static_cast<s32>(height)) {
            throw FormatError("N64 font: invalid glyph directory");
        }
        previous = code;
        glyphs.push_back({code, columns, x, y, 0, 0, columns, rows});
        // e0016d48 folds ASCII lowercase for fonts carrying flag1.
        if ((flags & 1U) && code >= 'A' && code <= 'Z') {
            glyphs.push_back({code + ('a' - 'A'), columns, x, y, 0, 0, columns, rows});
        }
    }
    LegendsN64Font result;
    result.font = BitmapFont::fromGlyphs(lineHeight, space, std::move(glyphs));
    result.atlas.width = width;
    result.atlas.height = height;
    result.atlas.pixels.resize(texels * 4, 255);
    for (usize i = 0; i < texels; ++i) {
        const u8 packed = bytes[pixels + i / 2];
        const u8 intensity = (i & 1) ? packed & 15U : packed >> 4U;
        result.atlas.pixels[i * 4 + 3] = intensity * 17U;
    }
    return result;
}

} // namespace gdl::formats
