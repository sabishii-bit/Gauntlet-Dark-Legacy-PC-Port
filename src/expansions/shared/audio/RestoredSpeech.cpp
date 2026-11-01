#include "expansions/shared/audio/RestoredSpeech.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <span>
#include <utility>

namespace gdl {
namespace {
constexpr usize kMaximumClips = 64;
constexpr usize kInputFrames = 1024;
constexpr usize kMaximumOutputFrames = 32768;
// Even a source supplying one frame per call reaches the two filters' maximum
// lookahead plus a full output block within this budget. Empty clips are bounded
// separately; a source returning true without data is invalid, not temporary EOF.
constexpr usize kMaximumPumps = 65536;
} // namespace

RestoredSpeech::RestoredSpeech(std::vector<std::shared_ptr<StreamSource>> clips) {
    if (clips.empty() || clips.size() > kMaximumClips) {
        return;
    }
    AudioStreamDesc format{0, 0};
    try {
        for (const auto& clip : clips) {
            if (!clip) {
                return;
            }
            const auto value = clip->desc();
            if (value.sampleRate < 1000 || value.sampleRate > 192000 || value.channels < 1 ||
                value.channels > 2 ||
                (format.sampleRate != 0 &&
                 (value.sampleRate != format.sampleRate || value.channels != format.channels))) {
                return;
            }
            format = value;
        }
    } catch (const std::exception&) {
        return;
    }
    m_desc = format;
    m_clips = std::move(clips);
    rewind();
    if (m_failed) {
        m_desc = {0, 0};
        m_clips.clear();
    }
}

bool RestoredSpeech::beginClip() {
    if (m_clip >= m_clips.size()) {
        return false;
    }
    m_clips[m_clip]->rewind();
    const auto format = m_clips[m_clip]->desc();
    if (format.sampleRate != m_desc.sampleRate || format.channels != m_desc.channels) {
        m_failed = true;
        return false;
    }
    m_finishedClip = false;
    if (m_desc.sampleRate >= 8000) {
        m_impulses.reset(m_desc.sampleRate, m_desc.channels, MusicDeclicker::Pass::Impulses);
        m_bursts.reset(m_desc.sampleRate, m_desc.channels, MusicDeclicker::Pass::ShortBursts);
    }
    return true;
}

void RestoredSpeech::rewind() {
    m_clip = m_cursor = 0;
    m_finishedClip = m_failed = false;
    m_input.clear();
    m_intermediate.clear();
    m_ready.clear();
    try {
        beginClip();
    } catch (const std::exception&) {
        m_failed = true;
    }
}

bool RestoredSpeech::pump() {
    m_ready.clear();
    m_cursor = 0;
    if (m_clip >= m_clips.size()) {
        return false;
    }
    if (m_finishedClip) {
        ++m_clip;
        if (!beginClip()) {
            return false;
        }
    }
    m_input.clear();
    const bool more = m_clips[m_clip]->read(m_input, kInputFrames);
    if ((more && m_input.empty()) || m_input.size() > kInputFrames * m_desc.channels ||
        m_input.size() % m_desc.channels != 0 ||
        !std::ranges::all_of(m_input, [](f32 value) { return std::isfinite(value); })) {
        m_failed = true;
        return false;
    }
    m_finishedClip = !more;
    if (m_desc.sampleRate < 8000) {
        m_ready = m_input;
    } else {
        m_intermediate.clear();
        m_impulses.feed(m_input, m_intermediate);
        if (m_finishedClip) {
            m_impulses.finish(m_intermediate);
        }
        m_bursts.feed(m_intermediate, m_ready);
        if (m_finishedClip) {
            m_bursts.finish(m_ready);
        }
    }
    return true;
}

bool RestoredSpeech::read(std::vector<f32>& output, usize frames) {
    if (m_failed || m_desc.channels == 0 || frames == 0) {
        return false;
    }
    const usize wanted = std::min(frames, kMaximumOutputFrames) * m_desc.channels;
    if (wanted > output.max_size() - output.size()) {
        return false;
    }
    usize appended = 0;
    usize pumps = 0;
    try {
        while (appended < wanted) {
            if (m_cursor == m_ready.size()) {
                if (++pumps > kMaximumPumps) {
                    m_failed = true;
                    break;
                }
                if (!pump()) {
                    break;
                }
                continue;
            }
            const usize count = std::min(wanted - appended, m_ready.size() - m_cursor);
            const auto available = std::span(m_ready).subspan(m_cursor, count);
            output.insert(output.end(), available.begin(), available.end());
            m_cursor += count;
            appended += count;
        }
    } catch (const std::exception&) {
        m_failed = true;
    }
    return appended != 0;
}

} // namespace gdl
