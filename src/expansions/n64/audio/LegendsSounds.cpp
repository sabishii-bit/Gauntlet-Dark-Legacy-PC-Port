#include "expansions/n64/audio/LegendsSounds.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/io/File.h"
#include "engine/platform/Paths.h"

#include "expansions/n64/audio/LegendsAudioMix.h"

namespace gdl::game {
namespace {
using Calibrations = std::unordered_map<u16, legends_audio_mix::Calibration>;

const Calibrations& nativeCalibrations(N64LegendsEffects& bank) {
    // open() authenticates every native US bank file before this is called.
    // Only immutable measurements are shared, never a synthesizer or cursor.
    // Function-local initialization serializes concurrent map loaders and
    // retries after a failed initialization. Scan during loading: a cold cue
    // took up to 27 ms, which must not become a hitch on its first combat hit.
    static const Calibrations measured = [&bank] {
        Calibrations result;
        result.reserve(bank.cueIds().size());
        for (const u16 id : bank.cueIds()) {
            auto source = bank.create(id);
            if (!source)
                throw FormatError("cannot prepare native N64 effect calibration");
            const auto calibration =
                legends_audio_mix::calibrate(*source, legends_audio_mix::Source::Effect);
            if (!calibration)
                throw FormatError("cannot measure native N64 effect calibration");
            result.emplace(id, *calibration);
        }
        return result;
    }();
    return measured;
}
} // namespace

bool LegendsSounds::openN64(const std::filesystem::path& root) {
    m_entries.clear();
    m_calibrations.clear();
    try {
        FileStream file(root / "_program/game.bin");
        if (file.size() < 0x7b514 - 0x400 || file.size() > u64{4} * 1024 * 1024) {
            throw FormatError("unsupported native N64 sound routing program");
        }
        const auto program = file.readExact(static_cast<usize>(file.size()));
        auto entries = decodeLegendsSoundTable(program);
        if (!m_effects.open(root / "audio")) {
            return false;
        }
        m_calibrations = nativeCalibrations(m_effects);
        m_entries = std::move(entries);
        return true;
    } catch (const std::exception& error) {
        log::warn("Legends sound routing {}: {}", paths::utf8(root), error.what());
        return false;
    }
}

std::optional<u32> LegendsSounds::namedToken(std::string_view name) const {
    return legendsSoundToken(m_entries, name);
}

std::optional<LegendsSoundCue> LegendsSounds::resolve(u32 token) const {
    if (m_entries.empty() || !m_effects.opened() || token == 0xffff || token == 0xffffffff) {
        return std::nullopt;
    }
    LegendsSoundEntry entry;
    if ((token & 0x10000U) != 0) {
        if ((token & ~0x10fffU) != 0 || (token & 0xfffU) >= m_entries.size()) {
            return std::nullopt;
        }
        entry = m_entries[token & 0xfffU];
    } else {
        if (token > 0xffff) {
            return std::nullopt;
        }
        const auto found = std::ranges::find(m_entries, token, &LegendsSoundEntry::token);
        if (found != m_entries.end()) {
            entry = *found;
        }
    }
    if (entry.replacement != 0xffff) {
        token = entry.replacement;
    }
    // e007a25c routes 0x1000/0x4000 to streamed narration and 0x2000 to
    // no effect. Do not mask those into unrelated valid MusyX effect IDs.
    if ((token & 0x7000U) != 0 || !std::isfinite(entry.pitch) || entry.pitch <= 0 ||
        entry.pitch > 8) {
        return std::nullopt;
    }
    const auto effect = static_cast<u16>(token & 0x3ffU);
    if (!m_effects.contains(effect)) {
        return std::nullopt;
    }
    // e000d0c8 quantizes game byte controls to MusyX's seven-bit controls.
    const auto volume = static_cast<u32>(static_cast<f32>(entry.volume) / 255.0f * 127.0f);
    const f32 pan = static_cast<f32>(static_cast<s32>(entry.pan) - 128) / 128.0f;
    const auto panByte = static_cast<u32>((pan + 1.0f) * .5f * 127.0f);
    return LegendsSoundCue{effect, static_cast<f32>(volume) / 127.0f,
                           std::clamp((static_cast<f32>(panByte) - 64.0f) / 63.0f, -1.0f, 1.0f),
                           entry.pitch};
}

SoundHandle LegendsSounds::play(SoundPlayer& output, u32 token, f32 volume, bool loop) {
    if (!std::isfinite(volume) || volume <= 0) {
        return kNoSound;
    }
    const auto cue = resolve(token);
    if (!cue) {
        return kNoSound;
    }
    const auto calibration = m_calibrations.find(cue->effect);
    if (calibration == m_calibrations.end())
        return kNoSound;
    auto source = m_effects.create(cue->effect, cue->pitch, cue->volume, cue->pan);
    if (!source) {
        return kNoSound;
    }
    // Bosses, ambient emitters and fixtures share the same PCM calibration.
    // Authored gain/pan/pitch remain in the native macro; distance and the user
    // category slider remain downstream and are never normalized away.
    source = legends_audio_mix::prepare(std::move(source), legends_audio_mix::Source::Effect,
                                        calibration->second);
    return output.playStream(std::move(source), loop, std::min(volume, 1.0f),
                             SoundCategory::Effects);
}

} // namespace gdl::game
