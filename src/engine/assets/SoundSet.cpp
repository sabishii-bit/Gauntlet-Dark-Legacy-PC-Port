#include "engine/assets/SoundSet.h"

#include <exception>

#include <nlohmann/json.hpp>

#include "engine/assets/WavFile.h"
#include "engine/core/Assert.h"
#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

namespace gdl {

namespace {

constexpr std::string_view kManifestName = "sounds.json";
constexpr f32 kMaxVolume = 127.0f;
constexpr f32 kSampleScale = 1.0f / 32768.0f;

} // namespace

bool SoundSet::load(const std::filesystem::path& directory) {
    m_entries.clear();
    m_byName.clear();
    m_samples.clear();
    const std::filesystem::path manifest = directory / kManifestName;
    try {
        const std::vector<u8> bytes = readFile(manifest);
        const nlohmann::json root = nlohmann::json::parse(bytes.begin(), bytes.end());
        for (const nlohmann::json& sound : root.at("sounds")) {
            SoundSetEntry entry;
            entry.name = sound.at("name").get<std::string>();
            entry.id = sound.value("id", 0U);
            entry.volume = static_cast<f32>(sound.value("volume", 127)) / kMaxVolume;
            entry.duration = sound.value("duration", 0.0f);
            if (sound.contains("sequence")) {
                for (const nlohmann::json& step : sound.at("sequence")) {
                    SoundSetStep s;
                    s.sample = step.at("sample").get<u32>();
                    s.loopStart = step.value("loopStart", false);
                    s.loopBack = step.value("loopBack", false);
                    entry.sequence.push_back(s);
                }
            }
            m_entries.push_back(std::move(entry));
        }
        for (const nlohmann::json& sample : root.at("samples")) {
            SampleInfo info;
            info.file = directory / sample.at("file").get<std::string>();
            m_samples.push_back(std::move(info));
        }
    } catch (const std::exception& e) {
        log::warn("Sound set {}: {}", manifest.string(), e.what());
        m_entries.clear();
        m_samples.clear();
        return false;
    }
    for (u32 i = 0; i < m_entries.size(); ++i) {
        m_byName.try_emplace(m_entries[i].name, i);
    }
    return !m_entries.empty();
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
        const WavData wav = loadWav(info.file);
        info.clip.sampleRate = wav.sampleRate;
        info.clip.channels = wav.channels;
        info.clip.samples.resize(wav.samples.size());
        for (usize i = 0; i < wav.samples.size(); ++i) {
            info.clip.samples[i] = static_cast<f32>(wav.samples[i]) * kSampleScale;
        }
    }
    return info.clip;
}

SoundSequence SoundSet::sequence(u32 index) {
    const SoundSetEntry& sound = entry(index);
    SoundSequence out;
    out.volume = sound.volume;
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
