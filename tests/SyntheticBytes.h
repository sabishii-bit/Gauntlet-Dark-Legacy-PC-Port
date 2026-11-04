#pragma once

#include <algorithm>
#include <bit>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/io/ByteReader.h"

namespace gdl::test {

/** Builds synthetic native files and streams: appends values to bytes it owns, little-endian
 * unless the writer's name ends in BE. */
class SyntheticBytes {
public:
    SyntheticBytes& putU8(u8 value) {
        m_bytes.push_back(value);
        return *this;
    }

    SyntheticBytes& putU16(u16 value) {
        putU8(static_cast<u8>(value & 0xFFU));
        return putU8(static_cast<u8>(value >> 8U));
    }

    SyntheticBytes& putU32(u32 value) {
        putU16(static_cast<u16>(value & 0xFFFFU));
        return putU16(static_cast<u16>(value >> 16U));
    }

    SyntheticBytes& putS32(s32 value) { return putU32(static_cast<u32>(value)); }

    SyntheticBytes& putF32(f32 value) { return putU32(std::bit_cast<u32>(value)); }

    SyntheticBytes& putU16BE(u16 value) {
        putU8(static_cast<u8>(value >> 8U));
        return putU8(static_cast<u8>(value & 0xFFU));
    }

    SyntheticBytes& putU32BE(u32 value) {
        putU16BE(static_cast<u16>(value >> 16U));
        return putU16BE(static_cast<u16>(value & 0xFFFFU));
    }

    SyntheticBytes& putFourcc(std::string_view code) { return putU32(fourcc(code)); }

    SyntheticBytes& putBytes(std::span<const u8> bytes) {
        m_bytes.insert(m_bytes.end(), bytes.begin(), bytes.end());
        return *this;
    }

    SyntheticBytes& putText(std::string_view text) {
        for (const char c : text) {
            putU8(static_cast<u8>(c));
        }
        return *this;
    }

    SyntheticBytes& putZeros(usize count) {
        m_bytes.insert(m_bytes.end(), count, u8{0});
        return *this;
    }

    const std::vector<u8>& bytes() const { return m_bytes; }
    usize size() const { return m_bytes.size(); }

private:
    std::vector<u8> m_bytes;
};

/** The `size` bytes at `offset` that a field writer below fills. A field that does not fit throws
 * std::out_of_range, which fails the case without adding an assertion. */
inline std::span<u8> fieldAt(std::span<u8> bytes, usize offset, usize size) {
    if (offset > bytes.size() || size > bytes.size() - offset) {
        throw std::out_of_range("a synthetic field lies past the end of its bytes");
    }
    return bytes.subspan(offset, size);
}

/** Field writers over a buffer already sized: each sets the value at `offset`, mirroring
 * readU16LE(bytes, offset) and its kin (engine/io/ByteReader.h). A field that does not fit throws
 * std::out_of_range before anything is written. */
inline void setU16LE(std::span<u8> bytes, usize offset, u16 value) {
    const std::span<u8> field = fieldAt(bytes, offset, sizeof(u16));
    field[0] = static_cast<u8>(value & 0xFFU);
    field[1] = static_cast<u8>(value >> 8U);
}

inline void setU32LE(std::span<u8> bytes, usize offset, u32 value) {
    const std::span<u8> field = fieldAt(bytes, offset, sizeof(u32));
    setU16LE(field, 0, static_cast<u16>(value & 0xFFFFU));
    setU16LE(field, sizeof(u16), static_cast<u16>(value >> 16U));
}

inline void setF32LE(std::span<u8> bytes, usize offset, f32 value) {
    setU32LE(bytes, offset, std::bit_cast<u32>(value));
}

inline void setU16BE(std::span<u8> bytes, usize offset, u16 value) {
    const std::span<u8> field = fieldAt(bytes, offset, sizeof(u16));
    field[0] = static_cast<u8>(value >> 8U);
    field[1] = static_cast<u8>(value & 0xFFU);
}

inline void setU32BE(std::span<u8> bytes, usize offset, u32 value) {
    const std::span<u8> field = fieldAt(bytes, offset, sizeof(u32));
    setU16BE(field, 0, static_cast<u16>(value >> 16U));
    setU16BE(field, sizeof(u16), static_cast<u16>(value & 0xFFFFU));
}

inline void setF32BE(std::span<u8> bytes, usize offset, f32 value) {
    setU32BE(bytes, offset, std::bit_cast<u32>(value));
}

/** Sets `text` into a fixed field of `fieldSize` bytes at `offset`, zeroing the rest of the field.
 * Text longer than the field throws std::length_error, and a field past the buffer throws
 * std::out_of_range, both before anything is written. */
inline void setText(std::span<u8> bytes, usize offset, usize fieldSize, std::string_view text) {
    if (text.size() > fieldSize) {
        throw std::length_error("synthetic text is longer than its field");
    }
    const std::span<u8> field = fieldAt(bytes, offset, fieldSize);
    std::ranges::fill(field, u8{0});
    std::ranges::transform(text, field.begin(), [](char c) { return static_cast<u8>(c); });
}

} // namespace gdl::test
