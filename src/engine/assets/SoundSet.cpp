#include "engine/assets/SoundSet.h"

#include "engine/audio/MusicDeclicker.h"
#include "engine/core/Assert.h"
#include "engine/core/Types.h"

namespace gdl {

namespace {

constexpr f32 kSampleScale = 1.0f / 32768.0f;

} // namespace

bool SoundSet::load(const std::filesystem::path& directory, Restoration restoration) {
    m_entries.clear();
    m_byName.clear();
    m_samples.clear();
    m_restoration = restoration;
    return loadNative(directory);
}

const SoundSetEntry& SoundSet::entry(u32 index) const {
    GDL_VERIFY(index < m_entries.size(), "sound index out of range");
    return m_entries[index];
}

std::optional<u32> SoundSet::find(std::string_view name) const {
    const auto it = m_byName.find(std::string(name));
    if (it == m_byName.end()) {
        return std::nullopt;
    }
    return it->second;
}

const SoundClip& SoundSet::sample(u32 index) {
    GDL_VERIFY(index < m_samples.size(), "sample index out of range");
    SampleInfo& info = m_samples[index];
    if (info.clip.samples.empty()) {
        const auto samples = formats::decodeBankSample(info.native);
        info.clip.sampleRate = info.native.sampleRate;
        info.clip.channels = 1;
        info.clip.samples.resize(samples.size());
        for (usize i = 0; i < samples.size(); ++i) {
            info.clip.samples[i] = static_cast<f32>(samples[i]) * kSampleScale;
        }
        if (m_restoration == Restoration::Enabled && info.clip.sampleRate >= 8000 &&
            info.clip.sampleRate <= 192000) {
            // Repair in sample space once, before playback resampling. Clip length,
            // sequence boundaries and the bank's loop points stay unchanged.
            MusicDeclicker filter;
            std::vector<f32> restored;
            filter.reset(info.clip.sampleRate, info.clip.channels);
            filter.feed(info.clip.samples, restored);
            filter.finish(restored);
            info.clip.samples.clear();
            filter.reset(info.clip.sampleRate, info.clip.channels,
                         MusicDeclicker::Pass::ShortBursts);
            filter.feed(restored, info.clip.samples);
            filter.finish(info.clip.samples);
        }
    }
    return info.clip;
}

SoundSequence SoundSet::sequence(u32 index) {
    const SoundSetEntry& sound = entry(index);
    SoundSequence out;
    out.volume = sound.volume;
    out.gainCurve = SoundGainCurve::Dcs;
    for (const SoundSetStep& step : sound.sequence) {
        if (step.sample >= m_samples.size()) {
            continue;
        }
        SoundSequenceStep s;
        s.clip = &sample(step.sample);
        s.loopStart = step.loopStart;
        s.loopBack = step.loopBack;
        out.steps.push_back(s);
    }
    return out;
}

} // namespace gdl
