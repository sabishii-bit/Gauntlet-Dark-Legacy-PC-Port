#include "game/world/LevelSoundscape.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <format>
#include <memory>
#include <utility>

#include "engine/audio/AdsStream.h"
#include "engine/audio/StreamPlaylist.h"
#include "engine/core/Log.h"
#include "engine/core/Types.h"

namespace gdl::game {
namespace {
constexpr std::array<std::string_view, 2> kStepSounds{"S_STEPROCK1", "S_STEPROCK2"};
constexpr std::string_view kPickupSound = "S_PICKUPMAGIC";
// sounds_evt.c's material table, not the order of sounds in any one bank.
// In CATHEDRAL the ICE slot contains the heavy-door creak recordings.
constexpr std::array<std::string_view, 6> kOpeningMaterials{"MET", "ROPE",  "CHAIN",
                                                            "ICE", "STONE", "*ROCK"};

} // namespace

void LevelSoundscape::open(const std::filesystem::path& root, SoundPlayer* output,
                           const LevelAudioInfo* info, char realm, bool boss) {
    close();
    m_realm = realm;
    m_boss = boss;
    m_output = output;
    if (info != nullptr) {
        m_level.load(root / "audio" / info->bank);
        m_stream = info->stream;
        // Only the initial music area is selected here.
        if (!m_stream.empty() && info->areas > 1) {
            m_stream += 'a';
        }
        m_streamParts = std::max(info->parts[0], 1);
    }
    m_ambient.load(root / "audio/TOWAMB");
    m_narrator.load(root / "audio/VOICE1");
    m_narratorSecond.load(root / "audio/VOICE2");
    m_promotions.load(root / "audio/WIZTOWER");
    if (m_common.load(root / "audio/COMMON")) {
        for (usize foot = 0; foot < kStepSounds.size(); ++foot) {
            m_steps[foot] = m_common.find(kStepSounds[foot]);
        }
        m_pickup = m_common.find(kPickupSound);
    }
}

void LevelSoundscape::bindAmbience(const WorldLayout& layout) {
    if (m_output != nullptr) {
        m_ambience.stop(*m_output);
    }
    const std::array<SoundSet*, 2> banks{&m_ambient, &m_level};
    m_ambience.bind(layout, banks);
}

void LevelSoundscape::updateAmbience(std::span<const Vec3> listeners, const AmbientEar& ear,
                                     f32 volume) {
    if (m_output != nullptr) {
        m_ambience.update(*m_output, listeners, ear, volume);
    }
}

void LevelSoundscape::startMusic(const AssetLocator* assets, f32 volume) {
    if (m_output == nullptr || assets == nullptr || m_stream.empty()) {
        return;
    }
    std::vector<std::unique_ptr<StreamSource>> parts;
    for (s32 i = 0; i < m_streamParts; ++i) {
        const std::string name =
            m_streamParts > 1 ? std::format("{}_{}", m_stream, i + 1) : m_stream;
        const auto file = assets->find(std::format("STREAMS/{}.ads", name));
        if (!file.has_value()) {
            log::warn("Level music stream {} is not among the game's files", name);
            return;
        }
        auto part = std::make_unique<AdsStream>();
        if (!part->open(*file)) {
            return;
        }
        parts.push_back(std::move(part));
    }
    std::shared_ptr<StreamPlaylist> stream;
    try {
        stream = std::make_shared<StreamPlaylist>(std::move(parts));
    } catch (const std::exception& e) {
        log::warn("Level music {}: {}", m_stream, e.what());
        return;
    }
    stop(m_music);
    m_music = m_output->playStream(std::move(stream), false, volume, SoundCategory::Music);
}

void LevelSoundscape::stop(SoundHandle handle) {
    if (m_output != nullptr && handle != kNoSound) {
        m_output->stop(handle);
    }
}

void LevelSoundscape::stopVoice() {
    stop(m_voice);
    m_voice = kNoSound;
}

void LevelSoundscape::stopCues() {
    stop(m_music);
    m_music = kNoSound;
    stopVoice();
    for (const Opening& sound : m_openings) {
        stop(sound.handle);
    }
    m_openings.clear();
}

void LevelSoundscape::suspend() {
    stopCues();
    for (const SoundHandle handle : m_voices) {
        stop(handle);
    }
    m_voices.clear();
    if (m_output != nullptr) {
        m_ambience.stop(*m_output);
    }
}

void LevelSoundscape::close() {
    suspend();
    m_ambience.clear();
    m_common = SoundSet{};
    m_level = SoundSet{};
    m_ambient = SoundSet{};
    m_narrator = SoundSet{};
    m_narratorSecond = SoundSet{};
    m_promotions = SoundSet{};
    m_steps.fill(std::nullopt);
    m_pickup.reset();
    m_stream.clear();
    m_streamParts = 1;
    m_output = nullptr;
}

SoundHandle LevelSoundscape::track(SoundHandle handle) {
    if (m_output != nullptr && handle != kNoSound) {
        std::erase_if(m_voices, [this](SoundHandle voice) { return !m_output->isPlaying(voice); });
        m_voices.push_back(handle);
    }
    return handle;
}

SoundHandle LevelSoundscape::playNamed(std::string_view name) {
    if (m_output == nullptr || name.empty()) {
        return kNoSound;
    }
    for (SoundSet* bank : {&m_level, &m_common, &m_ambient}) {
        if (const auto found = bank->find(name); found.has_value()) {
            try {
                return track(m_output->play(bank->sequence(*found), 1.0f, SoundCategory::Effects));
            } catch (const std::exception& e) {
                log::warn("Tower: sound {}: {}", name, e.what());
                return kNoSound;
            }
        }
    }
    return kNoSound;
}

SoundHandle LevelSoundscape::playFrom(SoundSet& bank, std::string_view name) {
    if (m_output != nullptr) {
        if (const auto found = bank.find(name); found.has_value()) {
            return track(m_output->play(bank.sequence(*found), 1.0f, SoundCategory::Effects));
        }
    }
    return kNoSound;
}

SoundHandle LevelSoundscape::playPromotion(std::string_view name, SoundHandle after) {
    if (m_output != nullptr) {
        if (const auto found = m_promotions.find(name); found.has_value()) {
            return track(m_output->playAfter(after, m_promotions.sequence(*found), 1.0f,
                                             SoundCategory::Effects));
        }
    }
    // The shared level-99 speech is in the main narrator bank, not WIZTOWER.
    const SoundHandle shared = narrate(name, Narrator::Primary, after);
    return shared != kNoSound ? shared : after;
}

SoundHandle LevelSoundscape::narrate(std::string_view name, Narrator which, SoundHandle after) {
    if (m_output == nullptr) {
        return kNoSound;
    }
    for (SoundSet* bank : {&m_narrator, &m_narratorSecond}) {
        if (const auto found = bank->find(name); found.has_value()) {
            return track(
                m_output->playAfter(after, bank->sequence(*found), 1.0f, SoundCategory::Effects));
        }
        if (which == Narrator::Primary) {
            break;
        }
    }
    return kNoSound;
}

void LevelSoundscape::playCommon(std::optional<u32> sound) {
    if (m_output != nullptr && sound.has_value()) {
        track(m_output->play(m_common.sequence(*sound), 1.0f, SoundCategory::Effects));
    }
}

void LevelSoundscape::playPickup() {
    playCommon(m_pickup);
}

void LevelSoundscape::playFootstep(bool second) {
    playCommon(m_steps[second ? 1 : 0]);
}

void LevelSoundscape::speakOverScroll(std::string_view name) {
    stopVoice();
    m_voice = playNamed(name);
}

void LevelSoundscape::opening(const TriggerOpening& event) {
    if (event.atOnce) {
        return;
    }
    if (const SoundHandle handle = playOpening(event.sound, false); handle != kNoSound) {
        m_openings.push_back(Opening{event.target, handle});
    }
}

void LevelSoundscape::settled(const TriggerOpening& event) {
    for (usize i = 0; i < m_openings.size();) {
        if (m_openings[i].target == event.target) {
            stop(m_openings[i].handle);
            m_openings.erase(m_openings.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    playOpening(event.sound, true);
}

SoundHandle LevelSoundscape::playOpening(s32 slot, bool settled) {
    if (slot < 0 || static_cast<usize>(slot) >= kOpeningMaterials.size()) {
        return kNoSound;
    }
    // GC 800a1614 constructs the sixth pair without a realm suffix, and
    // selects the B-suffixed elevator names in boss arenas.
    const auto material = kOpeningMaterials[static_cast<usize>(slot)];
    if (material.starts_with('*')) {
        return playNamed(std::format("S_{}{}", material.substr(1), settled ? "STOP" : "ROTATE"));
    }
    return playNamed(
        std::format("S_ELV{}{}{}{}", material, settled ? "STP" : "", m_realm, m_boss ? "B" : ""));
}

} // namespace gdl::game
