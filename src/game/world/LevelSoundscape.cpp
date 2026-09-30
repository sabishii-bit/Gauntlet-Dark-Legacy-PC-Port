#include "game/world/LevelSoundscape.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <format>
#include <memory>
#include <optional>
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
        m_streamAreas = std::max(info->areas, 1);
        m_streamParts = info->parts;
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
    m_areas.bind(layout);
}

void LevelSoundscape::updateAmbience(std::span<const Vec3> listeners, const AmbientEar& ear,
                                     f32 volume) {
    if (m_output != nullptr) {
        m_ambience.update(*m_output, listeners, ear, volume);
    }
}

void LevelSoundscape::updateMusicAreas(std::span<const Vec3> listeners) {
    if (const std::optional<MusicCue> cue = m_areas.update(listeners, m_musicArea);
        cue.has_value()) {
        selectMusicArea(cue->area, cue->how);
    }
}

s32 LevelSoundscape::partsOf(s32 area) const {
    if (area < 0 || static_cast<usize>(area) >= m_streamParts.size()) {
        return 1;
    }
    return std::max(m_streamParts[static_cast<usize>(area)], 1);
}

std::string LevelSoundscape::streamName(s32 area, s32 part) const {
    std::string name = m_stream;
    if (m_streamAreas > 1) {
        name += static_cast<char>('a' + area);
    }
    if (partsOf(area) > 1) {
        name += std::format("_{}", part + 1);
    }
    return name;
}

std::vector<std::unique_ptr<StreamSource>> LevelSoundscape::openArea(s32 area) const {
    std::vector<std::unique_ptr<StreamSource>> parts;
    if (m_assets == nullptr) {
        return parts;
    }
    for (s32 i = 0; i < partsOf(area); ++i) {
        const std::string name = streamName(area, i);
        const auto file = m_assets->find(std::format("STREAMS/{}.ads", name));
        if (!file.has_value()) {
            log::warn("Level music stream {} is not among the game's files", name);
            return {};
        }
        auto part = std::make_unique<AdsStream>();
        if (!part->open(*file)) {
            return {};
        }
        parts.push_back(std::move(part));
    }
    return parts;
}

void LevelSoundscape::startMusic(const AssetLocator* assets, f32 volume) {
    if (m_output == nullptr || assets == nullptr || m_stream.empty()) {
        return;
    }
    // BGMusicStart: the first area at full level, with nothing else asked for.
    m_assets = assets;
    m_musicVolume = volume;
    m_musicOn = true;
    m_musicArea = 0;
    m_musicSwitch = MusicSwitch::AtPartEnd;
    m_musicLevel = kFullLevel;
    m_musicFrames = 0.0f;
    playArea(0);
}

void LevelSoundscape::selectMusicArea(s32 area, MusicSwitch how) {
    m_musicArea = area;
    m_musicSwitch = how;
}

void LevelSoundscape::bossAwake(bool awake) {
    if (awake && !m_bossAwake) {
        selectMusicArea(1, MusicSwitch::Faded);
    }
    m_bossAwake = awake;
}

void LevelSoundscape::updateMusic(f32 seconds) {
    if (m_output == nullptr || !m_musicOn) {
        return;
    }
    m_musicFrames += seconds * kMusicRate;
    while (m_musicFrames >= 1.0f) {
        m_musicFrames -= 1.0f;
        stepMusic();
    }
}

void LevelSoundscape::stepMusic() {
    setupStreams();
    s32 target = kFullLevel;
    if (m_musicSwitch == MusicSwitch::Faded && m_musicArea != m_playingArea && m_streamAreas > 1) {
        target = m_musicLevel > kFadedLevel ? m_musicLevel - kFadeStep : m_musicLevel;
    }
    const s32 step = std::clamp(target - m_musicLevel, -kRiseStep, kRiseStep);
    if (step == 0) {
        return;
    }
    m_musicLevel += step;
    if (m_music != kNoSound) {
        m_output->setVolume(m_music, musicVolume());
    }
}

void LevelSoundscape::setupStreams() {
    if (m_playlist != nullptr && m_playlist->followed() != m_followsTaken) {
        // The switch at the part's end has gone through: the stream is the area it waited for.
        m_followsTaken = m_playlist->followed();
        m_playingArea = m_followArea;
        m_followArea = -1;
    }
    const s32 area = std::clamp(m_musicArea, 0, m_streamAreas - 1);
    if (m_music != kNoSound) {
        if (m_musicArea == m_playingArea) {
            if (m_followArea >= 0) {
                m_playlist->follow({});
                m_followArea = -1;
            }
            return;
        }
        if (m_musicSwitch == MusicSwitch::AtPartEnd) {
            // The original starts the area when its stream's end callback comes; the
            // playlist takes the area's parts over at that boundary instead.
            if (m_streamAreas > 1 && m_playlist != nullptr && m_followArea != area) {
                m_followArea = area;
                if (auto parts = openArea(area); !parts.empty()) {
                    try {
                        m_playlist->follow(std::move(parts));
                    } catch (const std::exception& e) {
                        log::warn("Level music {}: {}", streamName(area, 0), e.what());
                    }
                }
            }
            return;
        }
        if (m_musicSwitch == MusicSwitch::Faded && m_musicLevel > kFadedLevel) {
            return;
        }
        if (m_streamAreas <= 1) {
            m_musicArea = 0;
            m_musicSwitch = MusicSwitch::AtPartEnd;
            return;
        }
    } else if (m_musicSilent && m_musicArea == m_playingArea) {
        return;
    }
    playArea(area);
}

void LevelSoundscape::playArea(s32 area) {
    std::vector<std::unique_ptr<StreamSource>> parts = openArea(area);
    stopMusic();
    m_playingArea = area;
    // Playing an area's stream clears what was asked (sMusicSubState goes to nought).
    m_musicSwitch = MusicSwitch::AtPartEnd;
    m_musicSilent = parts.empty();
    if (parts.empty()) {
        return;
    }
    try {
        m_playlist = std::make_shared<StreamPlaylist>(std::move(parts));
    } catch (const std::exception& e) {
        log::warn("Level music {}: {}", streamName(area, 0), e.what());
        m_musicSilent = true;
        return;
    }
    m_music = m_output->playStream(m_playlist, false, musicVolume(), SoundCategory::Music);
}

f32 LevelSoundscape::musicVolume() const {
    return static_cast<f32>(m_musicLevel) / static_cast<f32>(kFullLevel) * m_musicVolume;
}

void LevelSoundscape::stopMusic() {
    stop(m_music);
    m_music = kNoSound;
    m_playlist.reset();
    m_followArea = -1;
    m_followsTaken = 0;
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
    // AudioStopSelect: the stream is let go of until the next start.
    stopMusic();
    m_musicOn = false;
    m_musicSilent = false;
    m_playingArea = -1;
    m_assets = nullptr;
    stopVoice();
    for (const Opening& sound : m_openings) {
        stop(sound.handle);
    }
    m_openings.clear();
}

void LevelSoundscape::suspend() {
    stopCues();
    clearNarration();
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
    m_areas.clear();
    m_common = SoundSet{};
    m_level = SoundSet{};
    m_ambient = SoundSet{};
    m_narrator = SoundSet{};
    m_narratorSecond = SoundSet{};
    m_promotions = SoundSet{};
    m_steps.fill(std::nullopt);
    m_pickup.reset();
    m_stream.clear();
    m_streamAreas = 1;
    m_streamParts.fill(0);
    m_musicVolume = 1.0f;
    m_musicArea = 0;
    m_musicSwitch = MusicSwitch::AtPartEnd;
    m_musicLevel = kFullLevel;
    m_musicFrames = 0.0f;
    m_bossAwake = false;
    m_output = nullptr;
}

SoundHandle LevelSoundscape::track(SoundHandle handle) {
    if (m_output != nullptr && handle != kNoSound) {
        std::erase_if(m_voices, [this](SoundHandle voice) { return !m_output->isPlaying(voice); });
        m_voices.push_back(handle);
    }
    return handle;
}

SoundHandle LevelSoundscape::playNamed(std::string_view name, f32 volume) {
    if (m_output == nullptr || name.empty()) {
        return kNoSound;
    }
    for (SoundSet* bank : {&m_level, &m_common, &m_ambient}) {
        if (const auto found = bank->find(name); found.has_value()) {
            try {
                return track(
                    m_output->play(bank->sequence(*found), volume, SoundCategory::Effects));
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

bool LevelSoundscape::narrationRoom(f32 maxWait) const {
    if (m_narrationHeld) {
        return false;
    }
    const f64 backlog = narrationBacklog();
    if (backlog > 0.0 && m_narrationEnds.size() >= kMostNarration) {
        return false;
    }
    return maxWait < 0.0f || backlog <= static_cast<f64>(maxWait);
}

f64 LevelSoundscape::narrationBacklog() const {
    // Lines stopped from elsewhere no longer wait, whatever the clock says.
    if (m_narrationEnds.empty() || m_output == nullptr || !m_output->isPlaying(m_narrationTail)) {
        return 0.0;
    }
    return std::max(m_narrationEnds.back() - m_narrationClock, 0.0);
}

SoundHandle LevelSoundscape::queueNarration(std::string_view name, Narrator which) {
    for (SoundSet* bank : {&m_narrator, &m_narratorSecond}) {
        if (const auto found = bank->find(name); found.has_value()) {
            return queue(*bank, *found);
        }
        if (which == Narrator::Primary) {
            return kNoSound;
        }
    }
    // Some of the narrator's lines live in the level's own banks (the tower's S_WAITINGL).
    for (SoundSet* bank : {&m_level, &m_common, &m_ambient}) {
        if (const auto found = bank->find(name); found.has_value()) {
            return queue(*bank, *found);
        }
    }
    return kNoSound;
}

SoundHandle LevelSoundscape::queueNarrationFrom(SoundSet& bank, std::string_view name) {
    const auto found = bank.find(name);
    return found.has_value() ? queue(bank, *found) : kNoSound;
}

SoundHandle LevelSoundscape::queue(SoundSet& bank, u32 sound) {
    if (m_output == nullptr || m_narrationHeld) {
        return kNoSound;
    }
    if (!m_output->isPlaying(m_narrationTail)) {
        m_narrationEnds.clear();
    }
    if (m_narrationEnds.size() >= kMostNarration) {
        return kNoSound;
    }
    const SoundSequence& sequence = bank.sequence(sound);
    const SoundHandle handle =
        track(m_output->playAfter(m_narrationTail, sequence, 1.0f, SoundCategory::Effects));
    if (handle == kNoSound) {
        return kNoSound;
    }
    f64 length = 0.0;
    for (const SoundSequenceStep& step : sequence.steps) {
        length += step.clip != nullptr ? step.clip->seconds() : 0.0;
    }
    const f64 start = m_narrationClock + narrationBacklog();
    m_narrationEnds.push_back(start + length);
    m_narrationTail = handle;
    return handle;
}

bool LevelSoundscape::announce(SoundSet& characterBank, std::string_view name, bool pojo,
                               std::span<const std::string_view> lines, f32 maxWait) {
    if (!narrationRoom(maxWait)) {
        return false;
    }
    if (pojo) {
        queueNarration(kPojoName);
    } else {
        queueNarrationFrom(characterBank, name);
    }
    for (const std::string_view line : lines) {
        queueNarration(line);
    }
    return true;
}

void LevelSoundscape::updateNarration(f32 seconds) {
    m_narrationClock += seconds;
    std::erase_if(m_narrationEnds, [this](f64 end) { return end <= m_narrationClock; });
}

void LevelSoundscape::clearNarration() {
    m_narrationEnds.clear();
    m_narrationTail = kNoSound;
    m_narrationHeld = false;
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
