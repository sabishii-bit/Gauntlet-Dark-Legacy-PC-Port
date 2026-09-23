#include "formats/WadDirectory.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>

#include "engine/core/Error.h"

namespace gdl::formats {

namespace {

constexpr std::size_t kHeaderSize = 16;
constexpr std::size_t kEntrySize = 16;
constexpr std::size_t kTagSize = 4;

void require(std::span<const std::uint8_t> bytes, std::size_t offset, std::size_t size,
             std::string_view what) {
    if (offset > bytes.size() || size > bytes.size() - offset) {
        throw FormatError(std::format("{}: truncated", what));
    }
}

} // namespace

std::uint32_t readWadU32(std::span<const std::uint8_t> bytes, std::size_t offset,
                         std::string_view what) {
    require(bytes, offset, 4, what);
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, 4);
    if constexpr (std::endian::native == std::endian::big) {
        value = std::byteswap(value);
    }
    return value;
}

std::uint16_t readWadU16(std::span<const std::uint8_t> bytes, std::size_t offset,
                         std::string_view what) {
    require(bytes, offset, 2, what);
    std::uint16_t value = 0;
    std::memcpy(&value, bytes.data() + offset, 2);
    if constexpr (std::endian::native == std::endian::big) {
        value = std::byteswap(value);
    }
    return value;
}

float readWadF32(std::span<const std::uint8_t> bytes, std::size_t offset, std::string_view what) {
    return std::bit_cast<float>(readWadU32(bytes, offset, what));
}

std::string readWadText(std::span<const std::uint8_t> bytes, std::size_t offset, std::size_t width,
                        std::string_view what) {
    require(bytes, offset, width, what);
    std::string text;
    for (std::size_t i = 0; i < width && bytes[offset + i] != 0; ++i) {
        text.push_back(static_cast<char>(bytes[offset + i]));
    }
    return text;
}

std::vector<WadSection> readWadDirectory(std::span<const std::uint8_t> bytes,
                                         std::string_view what) {
    if (bytes.size() < kHeaderSize) {
        throw FormatError(std::format("{}: too small", what));
    }
    const std::uint32_t directoryOffset = readWadU32(bytes, 0, what);
    const std::uint32_t sectionCount = readWadU32(bytes, 4, what);
    if (directoryOffset > bytes.size() ||
        sectionCount > (bytes.size() - directoryOffset) / kEntrySize) {
        throw FormatError(std::format("{}: bad directory", what));
    }
    std::vector<WadSection> sections;
    for (std::uint32_t i = 0; i < sectionCount; ++i) {
        const std::size_t entry = directoryOffset + std::size_t{i} * kEntrySize;
        WadSection section;
        for (std::size_t c = 0; c < kTagSize; ++c) {
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
