#pragma once

#include <array>
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
#ifndef GDL_TEST_UNPACKED_DIR
#define GDL_TEST_UNPACKED_DIR ""
#endif
#ifndef GDL_TEST_DATA_DIR
#define GDL_TEST_DATA_DIR ""
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

/** The gdlunpack output directory, or skips the current test when it has not been produced. */
inline std::filesystem::path unpackedOrSkip(std::string_view relative) {
    const std::filesystem::path path = std::filesystem::path(GDL_TEST_UNPACKED_DIR) / relative;
    if (!std::filesystem::exists(path)) {
        SKIP("unpacked asset " << relative << " is not available; run gdlunpack");
    }
    return path;
}

/** The repository's shipped data directory (configuration defaults and text). */
inline std::filesystem::path dataDirectory() {
    return {GDL_TEST_DATA_DIR};
}

/** A 2x2 RGBA PNG: red, green on the top row; blue, transparent white below. */
inline constexpr std::array<u8, 76> kTinyPng{
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xB6, 0x0D,
    0x24, 0x00, 0x00, 0x00, 0x13, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
    0x1F, 0x0C, 0x81, 0x34, 0x08, 0x30, 0x00, 0x00, 0x48, 0xC9, 0x08, 0xF8, 0x71, 0xC5, 0x31, 0xE0,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

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
