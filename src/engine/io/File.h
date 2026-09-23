#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gdl {

/** Reads a whole file into memory; throws FileError. */
std::vector<std::uint8_t> readFile(const std::filesystem::path& path);

/** Reads a whole file as text; throws FileError. */
std::string readTextFile(const std::filesystem::path& path);

/** Writes a whole file, replacing any existing one; throws FileError. */
void writeFile(const std::filesystem::path& path, std::span<const std::uint8_t> bytes);

/** Writes text as UTF-8 bytes, replacing any existing file; throws FileError. */
void writeTextFile(const std::filesystem::path& path, std::string_view text);

/** Sequential binary file access with seeking; every failure throws FileError. */
class FileStream {
public:
    explicit FileStream(const std::filesystem::path& path);

    std::uint64_t size() const { return m_size; }
    std::uint64_t position() const { return m_position; }
    const std::filesystem::path& path() const { return m_path; }

    void seek(std::uint64_t offset);

    /** Reads up to out.size() bytes; returns how many were read (fewer only at end of file). */
    std::size_t read(std::span<std::uint8_t> out);

    /** Reads exactly `count` bytes or throws. */
    std::vector<std::uint8_t> readExact(std::size_t count);

private:
    struct FileCloser {
        void operator()(std::FILE* file) const { std::fclose(file); }
    };

    std::filesystem::path m_path;
    std::unique_ptr<std::FILE, FileCloser> m_file;
    std::uint64_t m_size = 0;
    std::uint64_t m_position = 0;
};

} // namespace gdl
