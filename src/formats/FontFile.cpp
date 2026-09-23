#include "formats/FontFile.h"

#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {

namespace {

constexpr usize kHeaderSize = 12;
constexpr usize kGlyphSize = 16;

} // namespace

FontFile FontFile::parse(std::span<const u8> file) {
    if (file.size() < kHeaderSize) {
        throw FormatError("font file is too small for its header");
    }
    FontFile font;
    font.height = readS32LE(file, 4);
    if (font.height <= 0) {
        throw FormatError("font file has no glyph height");
    }
    for (usize at = kHeaderSize; at + kGlyphSize <= file.size(); at += kGlyphSize) {
        FontGlyph glyph;
        glyph.code = readS32LE(file, at);
        if (glyph.code == 0) {
            break;
        }
        glyph.width = readS32LE(file, at + 4);
        glyph.x = readS32LE(file, at + 8);
        glyph.y = readS32LE(file, at + 12);
        font.glyphs.push_back(glyph);
    }
    return font;
}

} // namespace gdl::formats
