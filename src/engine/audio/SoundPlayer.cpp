#include "engine/audio/SoundPlayer.h"

#include <algorithm>
#include <cmath>

namespace gdl {

bool SoundSequence::loops() const {
    return std::ranges::any_of(steps, [](const SoundSequenceStep& s) { return s.loopBack; });
}

SoundPlayer::SoundPlayer(AudioMixer& mixer) : m_mixer(mixer) {}

SoundHandle SoundPlayer::play(const SoundSequence& sequence, f32 volume) {
    if (sequence.steps.empty() || sequence.steps[0].clip == nullptr ||
        sequence.steps[0].clip->sampleRate == 0 || sequence.steps[0].clip->channels == 0) {
        return kNoSound;
    }
    Voice voice;
    voice.handle = m_nextHandle++;
    voice.sequence = sequence;
    const SoundClip& first = *sequence.steps[0].clip;
    voice.stream = m_mixer.createStream(AudioStreamDesc{first.sampleRate, first.channels});
    voice.stream->setVolume(std::clamp(volume * sequence.volume, 0.0f, 1.0f));
    feed(voice);
    m_voices.push_back(std::move(voice));
    return m_voices.back().handle;
}

void SoundPlayer::stop(SoundHandle handle) {
    for (Voice& voice : m_voices) {
        if (voice.handle == handle) {
            voice.stream->stop();
            voice.finished = true;
        }
    }
}

void SoundPlayer::stopAll() {
    for (Voice& voice : m_voices) {
        voice.stream->stop();
        voice.finished = true;
    }
}

bool SoundPlayer::isPlaying(SoundHandle handle) const {
    return std::ranges::any_of(
        m_voices, [handle](const Voice& v) { return v.handle == handle && !v.stream->drained(); });
}

void SoundPlayer::update() {
    for (Voice& voice : m_voices) {
        if (!voice.finished) {
            feed(voice);
        }
    }
    std::erase_if(m_voices, [](const Voice& v) { return v.stream->drained(); });
}

void SoundPlayer::feed(Voice& voice) {
    const std::vector<SoundSequenceStep>& steps = voice.sequence.steps;
    while (!voice.finished && voice.stream->queuedSeconds() < kLookaheadSeconds) {
        if (voice.nextStep >= steps.size()) {
            voice.stream->finish();
            voice.finished = true;
            break;
        }
        const usize current = voice.nextStep;
        const SoundSequenceStep& step = steps[current];
        if (step.clip != nullptr) {
            pushClip(*voice.stream, *step.clip);
        }
        if (step.loopBack) {
            usize target = current;
            while (target > 0 && !steps[target].loopStart) {
                --target;
            }
            voice.nextStep = target;
            if (step.clip == nullptr || step.clip->frames() == 0) {
                voice.stream->finish();
                voice.finished = true;
            }
        } else {
            voice.nextStep = current + 1;
        }
    }
}

void SoundPlayer::pushClip(AudioStream& stream, const SoundClip& clip) {
    const AudioStreamDesc& desc = stream.desc();
    if (clip.channels == desc.channels && clip.sampleRate == desc.sampleRate) {
        stream.push(clip.samples);
        return;
    }
    // Different rate or layout: resample linearly and fold the channels to the stream's.
    const usize frames = clip.frames();
    const auto outFrames = static_cast<usize>(std::llround(
        static_cast<f64>(frames) * desc.sampleRate / std::max<u32>(clip.sampleRate, 1)));
    std::vector<f32> out(outFrames * desc.channels);
    for (usize i = 0; i < outFrames; ++i) {
        const f64 source = static_cast<f64>(i) * clip.sampleRate / desc.sampleRate;
        const auto index = std::min(frames - 1, static_cast<usize>(source));
        const auto next = std::min(frames - 1, index + 1);
        const auto t = static_cast<f32>(source - static_cast<f64>(index));
        for (u32 c = 0; c < desc.channels; ++c) {
            const u32 sourceChannel = std::min(c, clip.channels - 1);
            const f32 a = clip.samples[index * clip.channels + sourceChannel];
            const f32 b = clip.samples[next * clip.channels + sourceChannel];
            out[i * desc.channels + c] = a + (b - a) * t;
        }
    }
    stream.push(out);
}

} // namespace gdl
