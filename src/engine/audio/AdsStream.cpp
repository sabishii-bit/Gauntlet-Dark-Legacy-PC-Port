#include "engine/audio/AdsStream.h"

#include <algorithm>
#include <exception>

#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

namespace gdl {

bool AdsStream::open(const std::filesystem::path& file, Restoration restoration) {
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
        // The final interleave round is a transport trailer, consumed but not played.
        const usize trailer = usize{m_info.blockSize} * m_info.channels;
        if (m_dataEnd - m_dataStart < trailer) {
            throw std::runtime_error("the file ends inside its transport trailer");
        }
        m_dataEnd -= trailer;
        m_restoration = restoration;
        if (m_restoration == Restoration::Enabled) {
            m_declicker.reset(m_info.sampleRate, m_info.channels);
            m_burstRepair.reset(m_info.sampleRate, m_info.channels,
                                MusicDeclicker::Pass::ShortBursts);
        }
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
    const u32 trailerFrames =
        m_info.blockSize / DspAdpcmDecoder::kFrameBytes * DspAdpcmDecoder::kSamplesPerFrame;
    return m_info.sampleRate == 0 || m_info.sampleCount < trailerFrames
               ? 0.0
               : static_cast<f64>(m_info.sampleCount - trailerFrames) / m_info.sampleRate;
}

AudioStreamDesc AdsStream::desc() const {
    return AudioStreamDesc{m_info.sampleRate, m_info.channels};
}

bool AdsStream::read(std::vector<f32>& out, usize frames) {
    if (!opened()) {
        return false;
    }
    const usize wanted = std::clamp<usize>(frames / DspAdpcmDecoder::kSamplesPerFrame *
                                               DspAdpcmDecoder::kFrameBytes * m_info.channels,
                                           1024, kPieceBytes);
    const usize before = out.size();
    while (out.size() == before && !m_flushed) {
        m_decoded.clear();
        if (m_offset < m_dataEnd) {
            const usize piece = std::min(wanted, m_dataEnd - m_offset);
            m_decoder.feed(std::span<const u8>(m_bytes).subspan(m_offset, piece), m_decoded);
            m_offset += piece;
        } else {
            m_decoder.flush(m_decoded);
            m_flushed = true;
        }
        if (m_restoration == Restoration::Disabled) {
            out.insert(out.end(), m_decoded.begin(), m_decoded.end());
            continue;
        }
        m_restored.clear();
        m_declicker.feed(m_decoded, m_restored);
        if (m_flushed) {
            m_declicker.finish(m_restored);
        }
        m_burstRepair.feed(m_restored, out);
        if (m_flushed) {
            m_burstRepair.finish(out);
        }
    }
    return out.size() != before;
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
    if (m_restoration == Restoration::Enabled) {
        m_declicker.reset(m_info.sampleRate, m_info.channels);
        m_burstRepair.reset(m_info.sampleRate, m_info.channels, MusicDeclicker::Pass::ShortBursts);
    }
}

} // namespace gdl
