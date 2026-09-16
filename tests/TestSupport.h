#pragma once

#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/ByteReader.h"

#ifndef GDL_TEST_ASSET_DIR
#define GDL_TEST_ASSET_DIR ""
#endif

namespace gdl::test {

/** A fresh, empty directory under the system temp folder. */
inline std::filesystem::path scratchDirectory(std::string_view name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "gdl-tests" / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

/** Locates a shipped asset, or skips the current test when the game data is absent. */
inline std::filesystem::path assetOrSkip(std::string_view relative) {
    const AssetLocator locator(GDL_TEST_ASSET_DIR);
    const auto found = locator.find(relative);
    if (!found.has_value()) {
        SKIP("asset " << relative << " is not installed");
    }
    return found.value_or(std::filesystem::path{});
}

/** Little-endian byte builder for synthetic files. */
class ByteWriter {
public:
    ByteWriter& putU8(u8 value) {
        m_bytes.push_back(value);
        return *this;
    }

    ByteWriter& putU16(u16 value) {
        putU8(static_cast<u8>(value & 0xFFU));
        return putU8(static_cast<u8>(value >> 8U));
    }

    ByteWriter& putU32(u32 value) {
        putU16(static_cast<u16>(value & 0xFFFFU));
        return putU16(static_cast<u16>(value >> 16U));
    }

    ByteWriter& putS32(s32 value) { return putU32(static_cast<u32>(value)); }

    ByteWriter& putFourcc(std::string_view code) { return putU32(fourcc(code)); }

    ByteWriter& putBytes(std::span<const u8> bytes) {
        m_bytes.insert(m_bytes.end(), bytes.begin(), bytes.end());
        return *this;
    }

    ByteWriter& putText(std::string_view text) {
        for (const char c : text) {
            putU8(static_cast<u8>(c));
        }
        return *this;
    }

    ByteWriter& putZeros(usize count) {
        m_bytes.insert(m_bytes.end(), count, u8{0});
        return *this;
    }

    const std::vector<u8>& bytes() const { return m_bytes; }
    usize size() const { return m_bytes.size(); }

private:
    std::vector<u8> m_bytes;
};

} // namespace gdl::test
