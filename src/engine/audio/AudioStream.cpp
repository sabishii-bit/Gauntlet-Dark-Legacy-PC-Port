#include "engine/audio/AudioStream.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>

#include "engine/core/Assert.h"
#include "engine/core/Types.h"

namespace gdl {

namespace {

/** The Catmull-Rom curve through four frames, `t` of the way from the second to the third. */
f32 curve(f32 p0, f32 p1, f32 p2, f32 p3, f32 t) {
    const f32 c1 = 0.5f * (p2 - p0);
    const f32 c2 = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
    const f32 c3 = 0.5f * (p3 - p0) + 1.5f * (p1 - p2);
    return ((c3 * t + c2) * t + c1) * t + p1;
}

/** A gain one step nearer its target. */
f32 slide(f32 level, f32 target, f32 step) {
    if (level < target) {
        return std::min(level + step, target);
    }
    return std::max(level - step, target);
}

} // namespace

AudioStream::AudioStream(AudioStreamDesc desc, u32 outputRate)
    : m_desc(desc), m_step(static_cast<f64>(desc.sampleRate) / outputRate),
      m_gainStep(1.0f / (kGainRamp * static_cast<f32>(outputRate))) {
    GDL_VERIFY(desc.channels > 0 && desc.sampleRate > 0 && outputRate > 0,
               "AudioStream needs non-zero channels and sample rates");
}

void AudioStream::push(std::span<const f32> interleaved) {
    const std::scoped_lock lock(m_mutex);
    m_queue.insert(m_queue.end(), interleaved.begin(), interleaved.end());
}

void AudioStream::finish() {
    const std::scoped_lock lock(m_mutex);
    m_finished = true;
}

void AudioStream::stop() {
    const std::scoped_lock lock(m_mutex);
    m_finished = true;
    m_stopping = true;
    // Enough of the queue is kept for the fade to run its course.
    const auto tail =
        static_cast<usize>(std::ceil(kGainRamp * static_cast<f32>(m_desc.sampleRate)));
    const usize keep = (m_readFrame + tail) * m_desc.channels;
    if (m_queue.size() > keep) {
        m_queue.resize(keep);
    }
}

bool AudioStream::finished() const {
    const std::scoped_lock lock(m_mutex);
    return m_finished;
}

bool AudioStream::drained() const {
    const std::scoped_lock lock(m_mutex);
    return m_finished && queuedFramesLocked() == 0;
}

f64 AudioStream::queuedSeconds() const {
    const std::scoped_lock lock(m_mutex);
    return static_cast<f64>(queuedFramesLocked()) / m_desc.sampleRate;
}

void AudioStream::setVolume(f32 volume) {
    const std::scoped_lock lock(m_mutex);
    m_volume = std::clamp(volume, 0.0f, 1.0f);
}

f32 AudioStream::volume() const {
    const std::scoped_lock lock(m_mutex);
    return m_volume;
}

void AudioStream::setPan(f32 pan) {
    const std::scoped_lock lock(m_mutex);
    m_pan = std::clamp(pan, -1.0f, 1.0f);
    const f32 angle = (m_pan + 1.0f) * 0.25f * std::numbers::pi_v<f32>;
    m_leftGain = std::cos(angle) * std::numbers::sqrt2_v<f32>;
    m_rightGain = std::sin(angle) * std::numbers::sqrt2_v<f32>;
}

f32 AudioStream::pan() const {
    const std::scoped_lock lock(m_mutex);
    return m_pan;
}

void AudioStream::mixInto(std::span<f32> stereoOut) {
    const std::scoped_lock lock(m_mutex);
    const usize channels = m_desc.channels;
    const usize outputFrames = stereoOut.size() / 2;
    const f32 targetLeft = m_stopping ? 0.0f : m_volume * m_leftGain;
    const f32 targetRight = m_stopping ? 0.0f : m_volume * m_rightGain;
    if (m_leftLevel < 0.0f) {
        // The first frame starts at its gain: the sound itself starts from silence.
        m_leftLevel = targetLeft;
        m_rightLevel = targetRight;
    }

    for (usize frame = 0; frame < outputFrames; ++frame) {
        const usize available = queuedFramesLocked();
        if (available == 0) {
            break;
        }
        // The curve runs through the frame before the read position and the two after it;
        // where the queue holds no such neighbour, the nearest frame stands in.
        const usize before = m_readFrame > 0 ? m_readFrame - 1 : m_readFrame;
        const usize next = available > 1 ? m_readFrame + 1 : m_readFrame;
        const usize after = available > 2 ? m_readFrame + 2 : next;
        const auto t = static_cast<f32>(m_fraction);
        const auto sample = [&](usize channel) {
            return curve(at(before, channel), at(m_readFrame, channel), at(next, channel),
                         at(after, channel), t);
        };
        const f32 left = sample(0);
        const f32 right = channels > 1 ? sample(1) : left;
        m_leftLevel = slide(m_leftLevel, targetLeft, m_gainStep);
        m_rightLevel = slide(m_rightLevel, targetRight, m_gainStep);
        stereoOut[frame * 2] += left * m_leftLevel;
        stereoOut[frame * 2 + 1] += right * m_rightLevel;

        m_fraction += m_step;
        while (m_fraction >= 1.0) {
            m_fraction -= 1.0;
            ++m_readFrame;
        }
    }
    compactLocked();
}

void AudioStream::compactLocked() {
    // Everything before the read position goes but the frame the curve still needs.
    if (m_readFrame < 2) {
        return;
    }
    const usize consumed = (m_readFrame - 1) * m_desc.channels;
    if (consumed < m_queue.size() / 2) {
        return;
    }
    m_queue.erase(m_queue.begin(), m_queue.begin() + static_cast<std::ptrdiff_t>(consumed));
    m_readFrame = 1;
}

} // namespace gdl
