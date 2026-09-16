#include "engine/io/File.h"

#include <cstring>
#include <format>
#include <system_error>

#include "engine/core/Error.h"

namespace gdl {

namespace {

std::FILE* openFile(const std::filesystem::path& path, const char* mode) {
    std::FILE* file = std::fopen(path.string().c_str(), mode);
    if (file == nullptr) {
        throw FileError(std::format("cannot open {}", path.string()));
    }
    return file;
}

} // namespace

std::vector<u8> readFile(const std::filesystem::path& path) {
    FileStream stream(path);
    return stream.readExact(static_cast<usize>(stream.size()));
}

void writeFile(const std::filesystem::path& path, std::span<const u8> bytes) {
    std::FILE* file = openFile(path, "wb");
    const usize written = std::fwrite(bytes.data(), 1, bytes.size(), file);
    const int closed = std::fclose(file);
    if (written != bytes.size() || closed != 0) {
        throw FileError(std::format("cannot write {}", path.string()));
    }
}

void writeTextFile(const std::filesystem::path& path, std::string_view text) {
    std::vector<u8> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    writeFile(path, bytes);
}

FileStream::FileStream(const std::filesystem::path& path)
    : m_path(path), m_file(openFile(path, "rb")) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        throw FileError(std::format("cannot size {}: {}", path.string(), ec.message()));
    }
    m_size = size;
}

void FileStream::seek(u64 offset) {
    if (offset > m_size) {
        throw FileError(std::format("seek past the end of {}", m_path.string()));
    }
    if (std::fseek(m_file.get(), static_cast<long>(offset), SEEK_SET) != 0) {
        throw FileError(std::format("seek failed in {}", m_path.string()));
    }
    m_position = offset;
}

usize FileStream::read(std::span<u8> out) {
    const usize count = std::fread(out.data(), 1, out.size(), m_file.get());
    if (count != out.size() && std::ferror(m_file.get()) != 0) {
        throw FileError(std::format("read failed in {}", m_path.string()));
    }
    m_position += count;
    return count;
}

std::vector<u8> FileStream::readExact(usize count) {
    std::vector<u8> bytes(count);
    if (read(bytes) != count) {
        throw FileError(std::format("unexpected end of {}", m_path.string()));
    }
    return bytes;
}

} // namespace gdl
