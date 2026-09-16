#include "engine/codec/AviReader.h"

#include <array>
#include <cctype>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

namespace gdl {

namespace {

constexpr u32 kRiff = fourcc("RIFF");
constexpr u32 kAvi = fourcc("AVI ");
constexpr u32 kList = fourcc("LIST");
constexpr u32 kHeaderList = fourcc("hdrl");
constexpr u32 kMovieList = fourcc("movi");
constexpr u32 kMainHeader = fourcc("avih");
constexpr u32 kStreamList = fourcc("strl");
constexpr u32 kStreamHeader = fourcc("strh");
constexpr u32 kStreamFormat = fourcc("strf");
constexpr u32 kJunk = fourcc("JUNK");
constexpr u32 kVideoStream = fourcc("vids");
constexpr u32 kAudioStream = fourcc("auds");
constexpr usize kChunkHeaderSize = 8;

usize padded(u32 size) {
    return usize{size} + (size & 1U);
}

AviStream parseStreamList(std::span<const u8> list) {
    AviStream stream;
    ByteReader reader(list);
    while (reader.remaining() >= kChunkHeaderSize) {
        const u32 id = reader.readU32();
        const u32 size = reader.readU32();
        const auto body = reader.readBytes(std::min(padded(size), reader.remaining()));
        if (id == kStreamHeader && body.size() >= 0x30) {
            stream.type = readU32LE(body, 0x00);
            stream.handler = readU32LE(body, 0x04);
            stream.scale = readU32LE(body, 0x14);
            stream.rate = readU32LE(body, 0x18);
            stream.length = readU32LE(body, 0x20);
            stream.sampleSize = readU32LE(body, 0x2C);
        } else if (id == kStreamFormat) {
            if (stream.type == kVideoStream && body.size() >= 0x14) {
                stream.video = AviVideoFormat{readU32LE(body, 0x04), readS32LE(body, 0x08),
                                              readU16LE(body, 0x0E), readU32LE(body, 0x10)};
            } else if (stream.type == kAudioStream && body.size() >= 0x10) {
                stream.audio = AviAudioFormat{readU16LE(body, 0x00), readU16LE(body, 0x02),
                                              readU32LE(body, 0x04), readU16LE(body, 0x0C),
                                              readU16LE(body, 0x0E)};
            }
        }
    }
    return stream;
}

/** Parses "01wb" style chunk ids into a stream index and kind. */
bool classifyChunk(u32 id, AviChunkKind& kind, u32& stream) {
    const auto d0 = static_cast<unsigned char>(id & 0xFFU);
    const auto d1 = static_cast<unsigned char>((id >> 8U) & 0xFFU);
    const auto t0 = static_cast<unsigned char>((id >> 16U) & 0xFFU);
    const auto t1 = static_cast<unsigned char>((id >> 24U) & 0xFFU);
    if (std::isdigit(d0) == 0 || std::isdigit(d1) == 0) {
        return false;
    }
    stream = (u32{d0} - '0') * 10 + (u32{d1} - '0');
    if (t0 == 'd' && (t1 == 'b' || t1 == 'c')) {
        kind = AviChunkKind::Video;
    } else if (t0 == 'w' && t1 == 'b') {
        kind = AviChunkKind::Audio;
    } else {
        kind = AviChunkKind::Other;
    }
    return true;
}

} // namespace

AviReader::AviReader(const std::filesystem::path& path) : m_file(path) {
    const auto riff = m_file.readExact(12);
    if (readU32LE(riff, 0) != kRiff || readU32LE(riff, 8) != kAvi) {
        throw FormatError("not a RIFF AVI file");
    }
    const u64 riffEnd = std::min<u64>(kChunkHeaderSize + readU32LE(riff, 4), m_file.size());

    u64 position = 12;
    bool foundMovie = false;
    while (position + kChunkHeaderSize <= riffEnd) {
        m_file.seek(position);
        const auto head = m_file.readExact(kChunkHeaderSize);
        const u32 id = readU32LE(head, 0);
        const u32 size = readU32LE(head, 4);
        if (id == kList && size >= 4) {
            const u32 type = readU32LE(m_file.readExact(4), 0);
            if (type == kHeaderList) {
                parseHeaderList(m_file.readExact(size - 4));
            } else if (type == kMovieList) {
                m_moviEnd = std::min<u64>(position + kChunkHeaderSize + size, riffEnd);
                m_file.seek(position + kChunkHeaderSize + 4);
                foundMovie = true;
                break;
            }
        }
        position += kChunkHeaderSize + padded(size);
    }
    if (!foundMovie) {
        throw FormatError("AVI file has no movie data");
    }
}

void AviReader::parseHeaderList(std::span<const u8> list) {
    ByteReader reader(list);
    while (reader.remaining() >= kChunkHeaderSize) {
        const u32 id = reader.readU32();
        const u32 size = reader.readU32();
        const auto body = reader.readBytes(std::min(padded(size), reader.remaining()));
        if (id == kMainHeader && body.size() >= 0x28) {
            m_header.microSecondsPerFrame = readU32LE(body, 0x00);
            m_header.totalFrames = readU32LE(body, 0x10);
            m_header.width = readU32LE(body, 0x20);
            m_header.height = readU32LE(body, 0x24);
        } else if (id == kList && body.size() >= 4 && readU32LE(body, 0) == kStreamList) {
            m_header.streams.push_back(parseStreamList(body.subspan(4)));
        }
    }
}

std::optional<AviChunk> AviReader::next() {
    std::array<u8, kChunkHeaderSize> head{};
    while (m_file.position() + kChunkHeaderSize <= m_moviEnd) {
        if (m_file.read(head) != head.size()) {
            return std::nullopt;
        }
        const u32 id = readU32LE(head, 0);
        const u32 size = readU32LE(head, 4);
        if (id == kList) {
            m_file.seek(m_file.position() + 4);
            continue;
        }
        const u64 nextChunk = m_file.position() + padded(size);
        AviChunk chunk;
        if (id == kJunk || !classifyChunk(id, chunk.kind, chunk.stream)) {
            m_file.seek(std::min(nextChunk, m_moviEnd));
            continue;
        }
        chunk.data = m_file.readExact(size);
        m_file.seek(std::min(nextChunk, m_moviEnd));
        return chunk;
    }
    return std::nullopt;
}

} // namespace gdl
