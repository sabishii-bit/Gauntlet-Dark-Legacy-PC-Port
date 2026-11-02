#pragma once

#include <bit>
#include <span>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

namespace gdl {

/** Appends little-endian values to a byte vector it does not own; ByteReader reads them back. */
class ByteWriter {
public:
    explicit ByteWriter(std::vector<u8>& bytes) : m_bytes(bytes) {}

    void writeU8(u8 value) { m_bytes.push_back(value); }

    void writeU16(u16 value) {
        writeU8(static_cast<u8>(value));
        writeU8(static_cast<u8>(value >> 8U));
    }

    void writeU32(u32 value) {
        writeU16(static_cast<u16>(value));
        writeU16(static_cast<u16>(value >> 16U));
    }

    /** The low word first, then the high one. */
    void writeU64(u64 value) {
        writeU32(static_cast<u32>(value));
        writeU32(static_cast<u32>(value >> 32U));
    }

    void writeS32(s32 value) { writeU32(static_cast<u32>(value)); }

    /** The value's IEEE-754 bits as a u32. */
    void writeF32(f32 value) { writeU32(std::bit_cast<u32>(value)); }

    void writeBytes(std::span<const u8> bytes) {
        m_bytes.insert(m_bytes.end(), bytes.begin(), bytes.end());
    }

    /** The characters alone, a byte each: no length and no terminator. */
    void writeString(std::string_view text) {
        m_bytes.insert(m_bytes.end(), text.begin(), text.end());
    }

private:
    std::vector<u8>& m_bytes;
};

} // namespace gdl
