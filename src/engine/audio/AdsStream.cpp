#include "engine/audio/AdsStream.h"

#include <algorithm>
#include <exception>

#include "engine/core/Log.h"
#include "engine/io/File.h"

namespace gdl {

bool AdsStream::open(const std::filesystem::path& file) {
    m_info = AdsAudioInfo{};
    m_bytes.clear();
    try {
        m_bytes = readFile(file);
        AdsAudioDecoder decoder;
        const auto header = decoder.parseHeader(m_bytes);
        if (!header.has_value()) {
            throw std::runtime_error("the file ends inside its header");
        }
        m_decoder = std::move(decoder);
        m_info = m_decoder.info();
        m_dataStart = *header;
        m_dataEnd = m_info.bodySize > 0 ? std::min(m_bytes.size(), m_dataStart + m_info.bodySize)
                                        : m_bytes.size();
    } catch (const std::exception& e) {
        log::warn("Audio stream {}: {}", file.string(), e.what());
        m_bytes.clear();
        m_info = AdsAudioInfo{};
        return false;
    }
    m_offset = m_dataStart;
    m_flushed = false;
    return true;
}

f64 AdsStream::seconds() const {
    return m_info.sampleRate == 0 ? 0.0
                                  : static_cast<f64>(m_info.sampleCount) / m_info.sampleRate;
}

AudioStreamDesc AdsStream::desc() const {
    return AudioStreamDesc{m_info.sampleRate, m_info.channels};
}

bool AdsStream::read(std::vector<f32>& out, usize frames) {
    if (!opened()) {
        return false;
    }
    if (m_offset >= m_dataEnd) {
        if (m_flushed) {
            return false;
        }
        m_flushed = true;
        const usize before = out.size();
        m_decoder.flush(out);
        return out.size() > before;
    }
    // Sixteen ADPCM bytes decode to fourteen frames per channel; take about `frames` worth.
    const usize wanted = std::clamp<usize>(frames / 14 * 16 * m_info.channels, 1024, kPieceBytes);
    const usize piece = std::min(wanted, m_dataEnd - m_offset);
    m_decoder.feed(std::span<const u8>(m_bytes).subspan(m_offset, piece), out);
    m_offset += piece;
    return true;
}

void AdsStream::rewind() {
    if (!opened()) {
        return;
    }
    AdsAudioDecoder fresh;
    fresh.parseHeader(m_bytes);
    m_decoder = std::move(fresh);
    m_offset = m_dataStart;
    m_flushed = false;
}

} // namespace gdl
