#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "engine/core/Types.h"
#include "engine/io/File.h"

namespace gdl {

struct AviVideoFormat {
    u32 width = 0;
    s32 height = 0; ///< negative means rows are stored top to bottom
    u16 bitCount = 0;
    u32 compression = 0; ///< FOURCC of the codec
};

struct AviAudioFormat {
    u16 formatTag = 0; ///< 1 = PCM
    u16 channels = 0;
    u32 samplesPerSecond = 0;
    u16 blockAlign = 0;
    u16 bitsPerSample = 0;
};

struct AviStream {
    u32 type = 0; ///< 'vids' or 'auds'
    u32 handler = 0;
    u32 scale = 0;
    u32 rate = 0; ///< rate / scale = frames (or samples) per second
    u32 length = 0;
    u32 sampleSize = 0;
    std::optional<AviVideoFormat> video;
    std::optional<AviAudioFormat> audio;
};

struct AviHeader {
    u32 microSecondsPerFrame = 0;
    u32 totalFrames = 0;
    u32 width = 0;
    u32 height = 0;
    std::vector<AviStream> streams;
};

enum class AviChunkKind : u8 { Video, Audio, Other };

struct AviChunk {
    AviChunkKind kind = AviChunkKind::Other;
    u32 stream = 0;
    std::vector<u8> data;
};

/** Streams the chunks of a RIFF AVI file in order. Throws FileError / FormatError. */
class AviReader {
public:
    explicit AviReader(const std::filesystem::path& path);

    const AviHeader& header() const { return m_header; }

    /** Returns the next data chunk of the movie, or nothing at the end. */
    std::optional<AviChunk> next();

private:
    void parseHeaderList(std::span<const u8> list);

    FileStream m_file;
    AviHeader m_header;
    u64 m_moviEnd = 0;
};

} // namespace gdl
