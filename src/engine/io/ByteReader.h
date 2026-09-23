#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "engine/core/Error.h"

namespace gdl {

/** Packs a four-character code the way RIFF stores it: first character in the low byte. */
constexpr std::uint32_t fourcc(std::string_view code) {
    return std::uint32_t{static_cast<std::uint8_t>(code[0])} |
           (std::uint32_t{static_cast<std::uint8_t>(code[1])} << 8U) |
           (std::uint32_t{static_cast<std::uint8_t>(code[2])} << 16U) |
           (std::uint32_t{static_cast<std::uint8_t>(code[3])} << 24U);
}

constexpr std::uint16_t readU16LE(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(std::uint32_t{bytes[offset]} |
                                      (std::uint32_t{bytes[offset + 1]} << 8U));
}

constexpr std::uint32_t readU32LE(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return std::uint32_t{bytes[offset]} | (std::uint32_t{bytes[offset + 1]} << 8U) |
           (std::uint32_t{bytes[offset + 2]} << 16U) | (std::uint32_t{bytes[offset + 3]} << 24U);
}

constexpr std::uint16_t readU16BE(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>((std::uint16_t{bytes[offset]} << 8U) |
                                      std::uint16_t{bytes[offset + 1]});
}

constexpr std::uint32_t readU32BE(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (std::uint32_t{readU16BE(bytes, offset)} << 16U) |
           std::uint32_t{readU16BE(bytes, offset + 2)};
}

constexpr std::int32_t readS32LE(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::int32_t>(readU32LE(bytes, offset));
}

/** Sequential little-endian reader over a byte span; throws FormatError on underflow. */
class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> bytes) : m_bytes(bytes) {}

    std::uint8_t readU8() {
        require(1);
        return m_bytes[m_position++];
    }

    std::uint16_t readU16() {
        require(2);
        const std::uint16_t value = readU16LE(m_bytes, m_position);
        m_position += 2;
        return value;
    }

    std::uint32_t readU32() {
        require(4);
        const std::uint32_t value = readU32LE(m_bytes, m_position);
        m_position += 4;
        return value;
    }

    std::int32_t readS32() { return static_cast<std::int32_t>(readU32()); }

    std::span<const std::uint8_t> readBytes(std::size_t count) {
        require(count);
        const auto view = m_bytes.subspan(m_position, count);
        m_position += count;
        return view;
    }

    void skip(std::size_t count) {
        require(count);
        m_position += count;
    }

    void seek(std::size_t position) {
        if (position > m_bytes.size()) {
            throw FormatError("seek past the end of the data");
        }
        m_position = position;
    }

    std::size_t position() const { return m_position; }
    std::size_t remaining() const { return m_bytes.size() - m_position; }
    bool atEnd() const { return m_position == m_bytes.size(); }

private:
    void require(std::size_t count) const {
        if (count > remaining()) {
            throw FormatError("read past the end of the data");
        }
    }

    std::span<const std::uint8_t> m_bytes;
    std::size_t m_position = 0;
};

} // namespace gdl
