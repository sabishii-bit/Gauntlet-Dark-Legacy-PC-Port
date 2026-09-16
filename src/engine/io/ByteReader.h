#pragma once

#include <span>
#include <string_view>

#include "engine/core/Error.h"
#include "engine/core/Types.h"

namespace gdl {

/** Packs a four-character code the way RIFF stores it: first character in the low byte. */
constexpr u32 fourcc(std::string_view code) {
    return u32{static_cast<u8>(code[0])} | (u32{static_cast<u8>(code[1])} << 8U) |
           (u32{static_cast<u8>(code[2])} << 16U) | (u32{static_cast<u8>(code[3])} << 24U);
}

constexpr u16 readU16LE(std::span<const u8> bytes, usize offset) {
    return static_cast<u16>(u32{bytes[offset]} | (u32{bytes[offset + 1]} << 8U));
}

constexpr u32 readU32LE(std::span<const u8> bytes, usize offset) {
    return u32{bytes[offset]} | (u32{bytes[offset + 1]} << 8U) | (u32{bytes[offset + 2]} << 16U) |
           (u32{bytes[offset + 3]} << 24U);
}

constexpr s32 readS32LE(std::span<const u8> bytes, usize offset) {
    return static_cast<s32>(readU32LE(bytes, offset));
}

/** Sequential little-endian reader over a byte span; throws FormatError on underflow. */
class ByteReader {
public:
    explicit ByteReader(std::span<const u8> bytes) : m_bytes(bytes) {}

    u8 readU8() {
        require(1);
        return m_bytes[m_position++];
    }

    u16 readU16() {
        require(2);
        const u16 value = readU16LE(m_bytes, m_position);
        m_position += 2;
        return value;
    }

    u32 readU32() {
        require(4);
        const u32 value = readU32LE(m_bytes, m_position);
        m_position += 4;
        return value;
    }

    s32 readS32() { return static_cast<s32>(readU32()); }

    std::span<const u8> readBytes(usize count) {
        require(count);
        const auto view = m_bytes.subspan(m_position, count);
        m_position += count;
        return view;
    }

    void skip(usize count) {
        require(count);
        m_position += count;
    }

    void seek(usize position) {
        if (position > m_bytes.size()) {
            throw FormatError("seek past the end of the data");
        }
        m_position = position;
    }

    usize position() const { return m_position; }
    usize remaining() const { return m_bytes.size() - m_position; }
    bool atEnd() const { return m_position == m_bytes.size(); }

private:
    void require(usize count) const {
        if (count > remaining()) {
            throw FormatError("read past the end of the data");
        }
    }

    std::span<const u8> m_bytes;
    usize m_position = 0;
};

} // namespace gdl
