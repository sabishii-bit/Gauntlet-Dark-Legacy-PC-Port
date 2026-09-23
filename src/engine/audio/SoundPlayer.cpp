#include "engine/audio/SoundPlayer.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Types.h"

namespace gdl {

bool SoundSequence::loops() const {
    return std::ranges::any_of(steps, [](const SoundSequenceStep& s) { return s.loopBack; });
}

SoundPlayer::SoundPlayer(AudioMixer& mixer) : m_mixer(mixer) {}

namespace {

bool playable(const SoundSequence& sequence) {
    return !sequence.steps.empty() && sequence.steps[0].clip != nullptr &&
           sequence.steps[0].clip->sampleRate != 0 && sequence.steps[0].clip->channels != 0;
}

} // namespace

SoundHandle SoundPlayer::play(const SoundSequence& sequence, f32 volume, SoundCategory category) {
    if (!playable(sequence)) {
        return kNoSound;
    }
    return start(sequence, volume, category, m_nextHandle++);
}

SoundHandle SoundPlayer::playAfter(SoundHandle previous, const SoundSequence& sequence, f32 volume,
                                   SoundCategory category) {
    if (!isPlaying(previous)) {
        return play(sequence, volume, category);
    }
    if (!playable(sequence)) {
        return kNoSound;
    }
    m_pending.push_back(Pending{m_nextHandle, previous, sequence, volume, category});
    return m_nextHandle++;
}

SoundHandle SoundPlayer::playStream(std::shared_ptr<StreamSource> source, bool loop, f32 volume,
                                    SoundCategory category) {
    if (source == nullptr || source->desc().sampleRate == 0 || source->desc().channels == 0) {
        return kNoSound;
    }
    Voice voice;
    voice.handle = m_nextHandle++;
    voice.stream = m_mixer.createStream(source->desc());
    voice.source = std::move(source);
    voice.loop = loop;
    voice.category = category;
    voice.volume = volume;
    applyVolume(voice);
    feed(voice);
    m_voices.push_back(std::move(voice));
    return m_voices.back().handle;
}

SoundHandle SoundPlayer::start(const SoundSequence& sequence, f32 volume, SoundCategory category,
                               SoundHandle handle) {
    Voice voice;
    voice.handle = handle;
    voice.sequence = sequence;
    const SoundClip& first = *sequence.steps[0].clip;
    voice.stream = m_mixer.createStream(AudioStreamDesc{first.sampleRate, first.channels});
    voice.category = category;
    voice.volume = volume * sequence.volume;
    applyVolume(voice);
    feed(voice);
    m_voices.push_back(std::move(voice));
    return m_voices.back().handle;
}

void SoundPlayer::setMasterVolume(f32 volume) {
    m_masterVolume = std::clamp(volume, 0.0f, 1.0f);
    for (Voice& voice : m_voices) {
        applyVolume(voice);
    }
}

void SoundPlayer::setCategoryVolume(SoundCategory category, f32 volume) {
    m_categoryVolumes[static_cast<usize>(category)] = std::clamp(volume, 0.0f, 1.0f);
    for (Voice& voice : m_voices) {
        applyVolume(voice);
    }
}

f32 SoundPlayer::categoryVolume(SoundCategory category) const {
    return m_categoryVolumes[static_cast<usize>(category)];
}

void SoundPlayer::applyVolume(Voice& voice) const {
    voice.stream->setVolume(
        std::clamp(m_masterVolume * categoryVolume(voice.category) * voice.volume, 0.0f, 1.0f));
}

void SoundPlayer::setVolume(SoundHandle handle, f32 volume) {
    for (Voice& voice : m_voices) {
        if (voice.handle == handle) {
            voice.volume = std::clamp(volume, 0.0f, 1.0f) * voice.sequence.volume;
            applyVolume(voice);
        }
    }
    for (Pending& pending : m_pending) {
        if (pending.handle == handle) {
            pending.volume = std::clamp(volume, 0.0f, 1.0f);
        }
    }
}

void SoundPlayer::setPan(SoundHandle handle, f32 pan) {
    for (const Voice& voice : m_voices) {
        if (voice.handle == handle) {
            voice.stream->setPan(pan);
        }
    }
}

void SoundPlayer::stop(SoundHandle handle) {
    for (Voice& voice : m_voices) {
        if (voice.handle == handle) {
            voice.stream->stop();
            voice.finished = true;
            voice.stopped = true;
        }
    }
    std::erase_if(m_pending, [handle](const Pending& p) { return p.handle == handle; });
}

void SoundPlayer::stopAll() {
    for (Voice& voice : m_voices) {
        voice.stream->stop();
        voice.finished = true;
        voice.stopped = true;
    }
    m_pending.clear();
}

bool SoundPlayer::isPlaying(SoundHandle handle) const {
    return std::ranges::any_of(m_voices,
                               [handle](const Voice& v) {
                                   return v.handle == handle && !v.stopped && !v.stream->drained();
                               }) ||
           std::ranges::any_of(m_pending,
                               [handle](const Pending& p) { return p.handle == handle; });
}

void SoundPlayer::update() {
    for (Voice& voice : m_voices) {
        if (!voice.finished) {
            feed(voice);
        }
    }
    std::erase_if(m_voices, [](const Voice& v) { return v.stream->drained(); });
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (isPlaying(it->after)) {
            ++it;
            continue;
        }
        const Pending pending = std::move(*it);
        it = m_pending.erase(it);
        start(pending.sequence, pending.volume, pending.category, pending.handle);
    }
}

void SoundPlayer::feedSource(Voice& voice) {
    const AudioStreamDesc desc = voice.source->desc();
    const auto pieceFrames = static_cast<usize>(desc.sampleRate / 2);
    while (!voice.finished && voice.stream->queuedSeconds() < kLookaheadSeconds) {
        m_scratch.clear();
        bool more = voice.source->read(m_scratch, pieceFrames);
        if (!more && voice.loop) {
            voice.source->rewind();
            more = voice.source->read(m_scratch, pieceFrames);
        }
        if (!more) {
            voice.stream->finish();
            voice.finished = true;
            break;
        }
        voice.stream->push(m_scratch);
    }
}

void SoundPlayer::feed(Voice& voice) {
    if (voice.source != nullptr) {
        feedSource(voice);
        return;
    }
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
