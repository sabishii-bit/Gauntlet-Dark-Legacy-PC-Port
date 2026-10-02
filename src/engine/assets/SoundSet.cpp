#include "engine/assets/SoundSet.h"

#include "engine/core/Assert.h"
#include "engine/core/Types.h"

namespace gdl {

namespace {

constexpr f32 kSampleScale = 1.0f / 32768.0f;

} // namespace

bool SoundSet::load(const std::filesystem::path& directory) {
    m_entries.clear();
    m_byName.clear();
    m_samples.clear();
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
