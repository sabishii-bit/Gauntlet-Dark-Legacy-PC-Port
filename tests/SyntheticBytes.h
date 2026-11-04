#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/io/ByteReader.h"

namespace gdl::test {

/** Builds synthetic native files and streams: appends little-endian values to bytes it owns. */
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

} // namespace gdl::test
