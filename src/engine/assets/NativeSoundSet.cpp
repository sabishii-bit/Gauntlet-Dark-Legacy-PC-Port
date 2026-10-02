#include <algorithm>
#include <exception>
#include <format>
#include <limits>
#include <utility>

#include "engine/assets/SoundSet.h"
#include "engine/codec/DspAdpcm.h"
#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/core/Strings.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"

#include "formats/AudioRom.h"
#include "formats/SoundBank.h"

namespace gdl {

bool SoundSet::loadNative(const std::filesystem::path& path) {
    try {
        const AssetLocator audio(path.parent_path());
        const bool explicitFile = toLowerAscii(path.extension().string()) == ".vbk";
        const auto name =
            normalizeAssetName(explicitFile ? path.stem().string() : path.filename().string());
        formats::AudioRom directory;
        if (const auto rom = audio.find("audatps2.rom")) {
            directory = formats::AudioRom::parse(readFile(*rom));
        }
        const formats::AudioRomBank* names = nullptr;
        for (const auto& bank : directory.banks) {
            if (normalizeAssetName(bank.name) == name || normalizeAssetName(bank.file) == name) {
                names = &bank;
                break;
            }
        }
        const auto filename =
            explicitFile ? path.filename().string() : (names ? names->file : name) + ".vbk";
        auto bank = formats::SoundBank::parse(readFile(audio.require(filename)));
        const usize namedCount = names ? names->soundCount : 0;
        if (names && static_cast<usize>(names->firstSound) + namedCount > directory.sounds.size()) {
            throw FormatError("bank sound names exceed audio directory");
        }
        for (usize i = 0; i < std::max(bank.calls.size(), namedCount); ++i) {
            SoundSetEntry entry;
            entry.id = std::numeric_limits<u32>::max();
            if (i < namedCount) {
                const auto& sound = directory.sounds[names->firstSound + i];
                entry.name = sound.name;
                entry.id = sound.id;
                entry.duration = sound.duration;
            } else {
                entry.name = std::format("{}_{:02}", name, i);
            }
            if (i < bank.calls.size()) {
                const auto& call = bank.calls[i];
                entry.volume = static_cast<f32>(call.volume) / formats::SoundBank::kMaxVolume;
                bool loops = false;
                for (const auto& step : call.steps) {
                    if (step.sample >= bank.samples.size()) {
                        throw FormatError("sound call sample index out of range");
                    }
                    entry.sequence.push_back({step.sample, step.loopStart, step.loopBack});
                    loops = loops || step.loopBack;
                    const auto& sample = bank.samples[step.sample];
                    if (i >= namedCount && sample.sampleRate != 0) {
                        auto count = sample.adpcm.size() / DspAdpcmDecoder::kFrameBytes *
                                     DspAdpcmDecoder::kSamplesPerFrame;
                        if (sample.sampleCount != 0) {
                            count = std::min(count, static_cast<usize>(sample.sampleCount));
                        }
                        entry.duration +=
                            static_cast<f32>(count) / static_cast<f32>(sample.sampleRate);
                    }
                }
                if (i >= namedCount && loops) {
                    entry.duration = -1.0f;
                }
            }
            m_byName.try_emplace(entry.name, static_cast<u32>(m_entries.size()));
            m_entries.push_back(std::move(entry));
        }
        for (auto& sample : bank.samples) {
            SampleInfo info;
            info.native = std::move(sample);
            m_samples.push_back(std::move(info));
        }
        return !m_entries.empty();
    } catch (const std::exception& e) {
        log::warn("Native sound bank {}: {}", path.string(), e.what());
        m_entries.clear();
        m_byName.clear();
        m_samples.clear();
        return false;
    }
}

} // namespace gdl
