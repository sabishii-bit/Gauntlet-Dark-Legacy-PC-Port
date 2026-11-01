#include "expansions/n64/audio/LegendsAudioMix.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace gdl::legendsAudioMix {
namespace {
struct Profile {
    f32 targetRms;
    f32 peakHeadroom;
    f32 maximumGain;
    u32 seconds;
};
std::optional<Profile> profile(Source kind) {
    // Final-mixer measurements at the ordinary .502 Effects slider put GDL
    // enemy cries at .136–.233 active RMS. Native cries formerly ranged from
    // .0215 to .119 despite identical unity cue controls. Aim for .16 after
    // that slider, retaining bounded transient headroom for the final limiter.
    // These are port mix targets, not recovered console master-volume values.
    switch (kind) {
    // At the default Music slider, the GDL cathedral tracks measure .121/.191
    // RMS through the real mixer. A .30 source target puts the corresponding
    // native tracks near .15, without discarding their transient headroom.
    case Source::Music: return Profile{.30f, 1.8f, 4.f, 30};
    case Source::Effect: return Profile{.32f, 1.8f, 16.f, 8};
    // Narration needs a little more presence than the effects bed. Town1/2
    // entry phrases already reached the old .30 target; a common +20% speech
    // target raises them without map-specific boosts or bypassing user volume.
    case Source::Speech: return Profile{.36f, 1.8f, 8.f, 30};
    }
    return {};
}
bool valid(const AudioStreamDesc& desc) {
    return desc.sampleRate > 0 && desc.sampleRate <= 192000 &&
           (desc.channels == 1 || desc.channels == 2);
}
class MixedSource final : public StreamSource {
public:
    MixedSource(std::shared_ptr<StreamSource> source, const Calibration& calibration)
        : m_source(std::move(source)), m_kind(calibration.kind), m_gain(calibration.gain) {}

    Source kind() const { return m_kind; }

    AudioStreamDesc desc() const override { return m_source->desc(); }

    bool read(std::vector<f32>& out, usize frames) override {
        const usize first = out.size();
        const bool more = m_source->read(out, frames);
        for (usize i = first; i < out.size(); ++i) {
            out[i] *= m_gain;
        }
        return more;
    }

    void rewind() override { m_source->rewind(); }

private:
    std::shared_ptr<StreamSource> m_source;
    Source m_kind;
    f32 m_gain;
};
} // namespace

std::optional<Calibration> calibrate(StreamSource& source, Source kind) {
    const auto policy = profile(kind);
    const auto desc = source.desc();
    if (!policy || !valid(desc))
        return {};
    Calibration result{.kind = kind};
    const usize limit = static_cast<usize>(desc.sampleRate) * policy->seconds;
    const usize window = std::max<usize>(1, desc.sampleRate / 100) * desc.channels;
    std::vector<f64> energies;
    std::vector<usize> sizes;
    std::vector<f32> block;
    f64 energy = 0;
    usize samples = 0;
    const auto finishWindow = [&] {
        if (samples != 0) {
            energies.push_back(energy);
            sizes.push_back(samples);
            energy = 0;
            samples = 0;
        }
    };
    try {
        while (result.frames < limit) {
            block.clear();
            const usize request = std::min<usize>(4096, limit - result.frames);
            const bool produced = source.read(block, request);
            if (block.size() > request * desc.channels || block.size() % desc.channels != 0 ||
                (produced && block.empty()))
                return {};
            for (const f32 value : block) {
                if (!std::isfinite(value))
                    return {};
                result.peak = std::max(result.peak, std::abs(value));
                energy += static_cast<f64>(value) * value;
                if (++samples == window)
                    finishWindow();
            }
            result.frames += block.size() / desc.channels;
            if (!produced) {
                result.complete = true;
                break;
            }
        }
        finishWindow();
        source.rewind();
    } catch (const std::exception&) {
        return {};
    }
    f64 loudest = 0;
    for (usize i = 0; i < energies.size(); ++i)
        loudest = std::max(loudest, energies[i] / static_cast<f64>(sizes[i]));
    // Ignore silence and long reverb tails, not individual zero crossings.
    // A relative -40 dB energy gate also preserves naturally quiet sources;
    // the absolute -60 dBFS floor prevents normalizing numerical noise.
    const f64 gate = std::max(.000001, loudest * .0001);
    f64 activeEnergy = 0;
    usize activeSamples = 0;
    for (usize i = 0; i < energies.size(); ++i) {
        if (energies[i] / static_cast<f64>(sizes[i]) > gate) {
            activeEnergy += energies[i];
            activeSamples += sizes[i];
        }
    }
    if (activeSamples != 0 && result.peak > 0) {
        result.activeRms = static_cast<f32>(std::sqrt(activeEnergy / activeSamples));
        result.gain = std::min({policy->targetRms / result.activeRms,
                                policy->peakHeadroom / result.peak, policy->maximumGain});
    }
    return result;
}

std::shared_ptr<StreamSource> prepare(std::shared_ptr<StreamSource> source, Source kind,
                                      std::optional<Calibration> calibration) {
    const auto policy = profile(kind);
    if (!source || !policy) {
        return {};
    }
    if (auto mixed = std::dynamic_pointer_cast<MixedSource>(source)) {
        return mixed->kind() == kind ? std::move(source) : nullptr;
    }
    if (!valid(source->desc())) {
        return {};
    }
    if (!calibration)
        calibration = calibrate(*source, kind);
    if (!calibration || calibration->kind != kind || !std::isfinite(calibration->gain) ||
        calibration->gain <= 0 || calibration->gain > policy->maximumGain)
        return {};
    return std::make_shared<MixedSource>(std::move(source), *calibration);
}

} // namespace gdl::legendsAudioMix
