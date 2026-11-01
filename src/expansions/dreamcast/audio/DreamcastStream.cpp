#include "expansions/dreamcast/audio/DreamcastStream.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <exception>

#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/io/ByteReader.h"
#include "engine/platform/Paths.h"

namespace gdl {

bool DreamcastStream::open(const std::filesystem::path& path) {
    m_file = {};
    m_desc = {0, 0};
    m_frames = 0;
    m_frame = 0;
    m_block.clear();
    try {
        m_file.open(path, std::ios::binary | std::ios::ate);
        const auto length = m_file.tellg();
        if (!m_file || length < 0x800) {
            throw FormatError("missing or truncated STR file");
        }
        m_file.seekg(0);
        std::array<u8, 0x800> header{};
        m_file.read(std::bit_cast<char*>(header.data()), header.size());
        const u32 tracks = readU32LE(header, 0);
        const u32 rate = readU32LE(header, 4);
        const u32 codec = readU32LE(header, 8);
        const u32 interleave = readU32LE(header, 12);
        const u32 blocks = readU32LE(header, 16);
        const u32 bytes = readU32LE(header, 20);
        const u32 channels = readU32LE(header, 24);
        const u64 capacity = static_cast<u64>(blocks) * channels * interleave;
        if (!m_file || std::memcmp(header.data() + 0xd5, "Sega Stream", 11) != 0 || tracks != 1 ||
            channels == 0 || channels > 2 || codec != 4 || rate < 8000 || rate > 192000 ||
            interleave == 0 || interleave > 65536 || interleave % 0x800 != 0 ||
            capacity != static_cast<u64>(length) - header.size() || bytes == 0 ||
            bytes > capacity || (static_cast<u64>(bytes) * 2) % channels != 0) {
            throw FormatError("unsupported or malformed Sega STR header");
        }
        m_desc = {rate, channels};
        m_interleave = interleave;
        m_frames = static_cast<u64>(bytes) * 2 / channels;
        m_block.resize(static_cast<usize>(interleave) * channels);
        rewind();
        return opened();
    } catch (const std::exception& error) {
        log::warn("Dreamcast stream {}: {}", paths::utf8(path), error.what());
        m_file = {};
        m_desc = {0, 0};
        m_frames = 0;
        m_block.clear();
        return false;
    }
}

f64 DreamcastStream::seconds() const {
    return opened() ? static_cast<f64>(m_frames) / m_desc.sampleRate : 0.0;
}

bool DreamcastStream::read(std::vector<f32>& out, usize frames) {
    if (!opened() || m_frame >= m_frames || frames == 0) {
        return false;
    }
    const usize wanted =
        static_cast<usize>(std::min<u64>(std::min<usize>(frames, 32768), m_frames - m_frame));
    out.reserve(out.size() + wanted * m_desc.channels);
    // AICA's standard Yamaha ADPCM scale factors. STR stores one planar
    // interleave per channel, low nibble first; state persists across blocks.
    // Independently checked against FFmpeg's dcstr/adpcm_aica output.
    constexpr std::array<s32, 8> kFactors{230, 230, 230, 230, 307, 409, 512, 614};
    for (usize frame = 0; frame < wanted; ++frame) {
        if (m_blockFrame == m_interleave * 2) {
            m_file.read(std::bit_cast<char*>(m_block.data()),
                        static_cast<std::streamsize>(m_block.size()));
            if (!m_file) {
                m_frame = m_frames;
                return frame != 0;
            }
            m_blockFrame = 0;
        }
        for (u32 channel = 0; channel < m_desc.channels; ++channel) {
            const u8 byte = m_block[channel * m_interleave + m_blockFrame / 2];
            const u8 nibble = (byte >> ((m_blockFrame & 1U) * 4U)) & 15U;
            auto& state = m_channels[channel];
            const s32 delta = state.step * (static_cast<s32>(nibble & 7U) * 2 + 1) / 8;
            state.sample =
                std::clamp(state.sample + ((nibble & 8U) != 0 ? -delta : delta), -32768, 32767);
            state.step = std::clamp(state.step * kFactors[nibble & 7U] / 256, 127, 24576);
            out.push_back(static_cast<f32>(state.sample) / 32768.0f);
        }
        ++m_blockFrame;
        ++m_frame;
    }
    return true;
}

void DreamcastStream::rewind() {
    if (!opened()) {
        return;
    }
    m_file.clear();
    m_file.seekg(0x800);
    m_channels = {};
    m_frame = 0;
    m_blockFrame = m_interleave * 2;
    if (!m_file) {
        m_desc = {0, 0};
    }
}

} // namespace gdl
