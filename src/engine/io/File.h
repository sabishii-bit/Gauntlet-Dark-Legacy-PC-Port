#pragma once

#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include "engine/core/Types.h"

namespace gdl {

/** Reads a whole file into memory; throws FileError. */
std::vector<u8> readFile(const std::filesystem::path& path);

/** Writes a whole file, replacing any existing one; throws FileError. */
void writeFile(const std::filesystem::path& path, std::span<const u8> bytes);

/** Sequential binary file access with seeking; every failure throws FileError. */
class FileStream {
public:
    explicit FileStream(const std::filesystem::path& path);

    u64 size() const { return m_size; }
    u64 position() const { return m_position; }
    const std::filesystem::path& path() const { return m_path; }

    void seek(u64 offset);

    /** Reads up to out.size() bytes; returns how many were read (fewer only at end of file). */
    usize read(std::span<u8> out);

    /** Reads exactly `count` bytes or throws. */
    std::vector<u8> readExact(usize count);

private:
    struct FileCloser {
        void operator()(std::FILE* file) const { std::fclose(file); }
    };

    std::filesystem::path m_path;
    std::unique_ptr<std::FILE, FileCloser> m_file;
    u64 m_size = 0;
    u64 m_position = 0;
};

} // namespace gdl
