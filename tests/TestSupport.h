#pragma once

#include <array>
#include <chrono>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"

#include "SyntheticBytes.h" // IWYU pragma: export

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

/** How long an entry of the scratch folder may go unwritten before a test process sweeps it. */
inline constexpr std::chrono::hours kStaleScratchAge{24};

/** The folder under the system temp folder that holds every test process's scratch root. */
inline std::filesystem::path scratchParent() {
    return std::filesystem::temp_directory_path() / "gdl-tests";
}

/** This process's id, which with its start names its scratch root. */
inline s64 processId() {
#ifdef _WIN32
    return _getpid();
#else
    return getpid();
#endif
}

/** The system clock in microseconds, read once as a process makes its scratch root: ids are
 * reused, so a process given the id of one that left its root behind still names its own apart. */
inline s64 clockCount() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/** The name of the scratch root that process `pid` made at clock count `start`. */
inline std::string scratchRootName(s64 pid, s64 start) {
    return "p" + std::to_string(pid) + "-" + std::to_string(start);
}

/** Removes the entries of `folder` last written more than kStaleScratchAge before `now`, other
 * than `keep`: the roots of runs that never exited and older fixed-name folders. A root is
 * written as it is made and no test process lives a day, so a running process's root stays; no
 * later process takes a root's name, so an entry found stale stays stale until it is removed. */
inline void sweepStaleScratch(const std::filesystem::path& folder,
                              std::filesystem::file_time_type now,
                              const std::filesystem::path& keep = {}) {
    std::vector<std::filesystem::path> stale;
    std::error_code error;
    for (auto entry = std::filesystem::directory_iterator(folder, error);
         !error && entry != std::filesystem::directory_iterator(); entry.increment(error)) {
        std::error_code unread;
        const auto written = entry->last_write_time(unread);
        if (!unread && entry->path() != keep && now - written > kStaleScratchAge) {
            stale.push_back(entry->path());
        }
    }
    for (const std::filesystem::path& path : stale) {
        std::error_code ignored; // another process may be removing it too
        std::filesystem::remove_all(path, ignored);
    }
}

/** This process's own folder under scratchParent(), `p<pid>-<start>`, so test processes running
 * at once never share fixtures and none takes the name of a root another left behind. Made when
 * first asked for, sweeping stale entries beside it, and removed when the process exits. */
class ScratchRoot {
public:
    ScratchRoot() : m_path(scratchParent() / scratchRootName(processId(), clockCount())) {
        std::filesystem::create_directories(m_path);
        sweepStaleScratch(m_path.parent_path(), std::filesystem::file_time_type::clock::now(),
                          m_path);
    }
    ~ScratchRoot() {
        std::error_code ignored; // a file still open at exit leaves the rest for a later sweep
        std::filesystem::remove_all(m_path, ignored);
    }
    GDL_NON_COPYABLE_NON_MOVABLE(ScratchRoot);

    const std::filesystem::path& path() const { return m_path; }

private:
    std::filesystem::path m_path;
};

/** This process's scratch root, made on first use and removed at exit. */
inline const std::filesystem::path& scratchRoot() {
    static const ScratchRoot kRoot;
    return kRoot.path();
}

/** A fresh, empty directory under this process's scratch root. */
inline std::filesystem::path scratchDirectory(std::string_view name) {
    const std::filesystem::path dir = scratchRoot() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

/** Locates a shipped asset, or skips the current test when the game data is absent.
 * Call before assertions: Catch2 treats a SKIP nested in REQUIRE as a failure. */
inline std::filesystem::path assetOrSkip(std::string_view relative) {
    const AssetLocator locator(GDL_TEST_ASSET_DIR);
    const auto found = locator.find(relative);
    if (!found.has_value()) {
        SKIP("asset " << relative << " is not installed");
    }
    return found.value_or(std::filesystem::path{});
}

/** The gdlunpack output directory, or skips the current test when it has not been produced.
 * Call before assertions, including when reached through another fixture helper. */
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

/** Lower-case hex of bytes, two digits a byte, for pinning an encoding to a literal. */
inline std::string hexBytes(std::span<const u8> bytes) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const u8 value : bytes) {
        text.push_back(kDigits[value >> 4U]);
        text.push_back(kDigits[value & 0xFU]);
    }
    return text;
}

} // namespace gdl::test
