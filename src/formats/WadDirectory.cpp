#include "formats/WadDirectory.h"

#include <bit>
#include <cstring>
#include <format>

#include "engine/core/Error.h"
#include "engine/core/Types.h"

namespace gdl::formats {

namespace {

constexpr usize kHeaderSize = 16;
constexpr usize kEntrySize = 16;
constexpr usize kTagSize = 4;

void require(std::span<const u8> bytes, usize offset, usize size, std::string_view what) {
    if (offset > bytes.size() || size > bytes.size() - offset) {
        throw FormatError(std::format("{}: truncated", what));
    }
}

} // namespace

u32 readWadU32(std::span<const u8> bytes, usize offset, std::string_view what) {
    require(bytes, offset, 4, what);
    u32 value = 0;
    std::memcpy(&value, bytes.data() + offset, 4);
    if constexpr (std::endian::native == std::endian::big) {
        value = std::byteswap(value);
    }
    return value;
}

u16 readWadU16(std::span<const u8> bytes, usize offset, std::string_view what) {
    require(bytes, offset, 2, what);
    u16 value = 0;
    std::memcpy(&value, bytes.data() + offset, 2);
    if constexpr (std::endian::native == std::endian::big) {
        value = std::byteswap(value);
    }
    return value;
}

f32 readWadF32(std::span<const u8> bytes, usize offset, std::string_view what) {
    return std::bit_cast<f32>(readWadU32(bytes, offset, what));
}

std::string readWadText(std::span<const u8> bytes, usize offset, usize width,
                        std::string_view what) {
    require(bytes, offset, width, what);
    std::string text;
    for (usize i = 0; i < width && bytes[offset + i] != 0; ++i) {
        text.push_back(static_cast<char>(bytes[offset + i]));
    }
    return text;
}

std::vector<WadSection> readWadDirectory(std::span<const u8> bytes, std::string_view what) {
    if (bytes.size() < kHeaderSize) {
        throw FormatError(std::format("{}: too small", what));
    }
    const u32 directoryOffset = readWadU32(bytes, 0, what);
    const u32 sectionCount = readWadU32(bytes, 4, what);
    if (directoryOffset > bytes.size() ||
        sectionCount > (bytes.size() - directoryOffset) / kEntrySize) {
        throw FormatError(std::format("{}: bad directory", what));
    }
    std::vector<WadSection> sections;
    for (u32 i = 0; i < sectionCount; ++i) {
        const usize entry = directoryOffset + usize{i} * kEntrySize;
        WadSection section;
        for (usize c = 0; c < kTagSize; ++c) {
            section.tag.push_back(static_cast<char>(bytes[entry + kTagSize - 1 - c]));
        }
        section.offset = readWadU32(bytes, entry + 4, what);
        section.count = readWadU32(bytes, entry + 8, what);
        if (section.offset > bytes.size()) {
            throw FormatError(
                std::format("{}: section {} lies outside the file", what, section.tag));
        }
        sections.push_back(std::move(section));
    }
    return sections;
}

const WadSection* findWadSection(std::span<const WadSection> sections, std::string_view tag) {
    for (const WadSection& section : sections) {
        if (section.tag == tag) {
            return &section;
        }
    }
    return nullptr;
}

} // namespace gdl::formats
