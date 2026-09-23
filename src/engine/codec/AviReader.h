#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

#include "engine/io/File.h"

namespace gdl {

struct AviVideoFormat {
    std::uint32_t width = 0;
    std::int32_t height = 0; ///< negative means rows are stored top to bottom
    std::uint16_t bitCount = 0;
    std::uint32_t compression = 0; ///< FOURCC of the codec
};

struct AviAudioFormat {
    std::uint16_t formatTag = 0; ///< 1 = PCM
    std::uint16_t channels = 0;
    std::uint32_t samplesPerSecond = 0;
    std::uint16_t blockAlign = 0;
    std::uint16_t bitsPerSample = 0;
};

struct AviStream {
    std::uint32_t type = 0; ///< 'vids' or 'auds'
    std::uint32_t handler = 0;
    std::uint32_t scale = 0;
    std::uint32_t rate = 0; ///< rate / scale = frames (or samples) per second
    std::uint32_t length = 0;
    std::uint32_t sampleSize = 0;
    std::optional<AviVideoFormat> video;
    std::optional<AviAudioFormat> audio;
};

struct AviHeader {
    std::uint32_t microSecondsPerFrame = 0;
    std::uint32_t totalFrames = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<AviStream> streams;
};

enum class AviChunkKind : std::uint8_t { Video, Audio, Other };

struct AviChunk {
    AviChunkKind kind = AviChunkKind::Other;
    std::uint32_t stream = 0;
    std::vector<std::uint8_t> data;
};

/** Streams the chunks of a RIFF AVI file in order. Throws FileError / FormatError. */
class AviReader {
public:
    explicit AviReader(const std::filesystem::path& path);

    const AviHeader& header() const { return m_header; }

    /** Returns the next data chunk of the movie, or nothing at the end. */
    std::optional<AviChunk> next();

private:
    void parseHeaderList(std::span<const std::uint8_t> list);

    FileStream m_file;
    AviHeader m_header;
    std::uint64_t m_moviEnd = 0;
};

} // namespace gdl
