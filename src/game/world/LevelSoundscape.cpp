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
// fn_8009EFCC's table (sounds_evt.c 90-93) by step kind, first and second foot.
constexpr std::array<std::string_view, 5> kStepKinds{"ROCK", "WOOD", "STAIR", "MET", "WATER"};
constexpr std::string_view kPickupSound = "S_PICKUPMAGIC";
constexpr std::string_view kEntranceSound = "S_ENTRANCE";
constexpr std::string_view kStunHint = "S_SHOTSSTUN";
constexpr std::string_view kGrabHint = "S_GRAB";
constexpr std::string_view kTrapMotion = "S_TRAP"; ///< plus the realm's letter
constexpr std::string_view kQuakeMotion = "S_QUAKEC";
constexpr std::string_view kClunkMotion = "S_ELVCNNK";
constexpr char kPyramidRealm = 'C';
constexpr char kSkyRealm = 'K';
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
    m_narration.bind(output);
    m_barks.bind(output);
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
    m_objectFlags.clear();
    m_objectFlags.reserve(layout.objects().size());
    for (const WorldObject& object : layout.objects()) {
        m_objectFlags.push_back(object.flags);
    }
}

void LevelSoundscape::updateAmbience(std::span<const Vec3> listeners, const AmbientEar& ear,
                                     f32 volume, bool ducked, const WorldScene* world) {
    m_ear = ear;
    if (m_output != nullptr) {
        m_ambience.update(*m_output, listeners, ear, volume,
                          ducked ? std::optional<f32>{kDuckedLevel} : std::nullopt, world);
        if (const auto scale = m_ambience.musicScale(); scale.has_value()) {
            duckMusic(AmbientSounds::kMusicDuckHold, *scale);
        }
    }
}

void LevelSoundscape::pauseAmbience() {
    if (m_output != nullptr) {
        m_ambience.stop(*m_output);
    }
    stopLoop(m_exitFlame);
    stopLoop(m_hourglass);
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
    m_musicDuck = 0.0f;
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

void LevelSoundscape::duckMusic(f32 seconds, f32 scale) {
    // AudioClampMusicVol: the scale is kept between a fifth and the whole.
    constexpr f32 kLeastScale = 0.2f;
    m_musicDuck = std::max(seconds, 0.0f);
    m_musicDuckScale = scale < kLeastScale ? kLeastScale : std::min(scale, 1.0f);
}

void LevelSoundscape::updateMusic(f32 seconds) {
    if (m_output == nullptr || !m_musicOn) {
        return;
    }
    m_musicDuck = std::max(m_musicDuck - seconds, 0.0f);
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
    } else if (m_musicDuck > 0.0f) {
        target = static_cast<s32>(static_cast<f32>(kFullLevel) * m_musicDuckScale);
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
    stopLoop(m_exitFlame);
    stopLoop(m_hourglass);
}

void LevelSoundscape::suspend() {
    stopCues();
    m_narration.clear();
    m_barks.clear();
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
    m_ear = {};
    m_ambience.clear();
    m_areas.clear();
    m_objectFlags.clear();
    m_common = SoundSet{};
    m_level = SoundSet{};
    m_ambient = SoundSet{};
    m_narrator = SoundSet{};
    m_narratorSecond = SoundSet{};
    m_promotions = SoundSet{};
    m_pickup.reset();
    m_stream.clear();
    m_streamAreas = 1;
    m_streamParts.fill(0);
    m_musicVolume = 1.0f;
    m_musicArea = 0;
    m_musicSwitch = MusicSwitch::AtPartEnd;
    m_musicLevel = kFullLevel;
    m_musicFrames = 0.0f;
    m_musicDuck = 0.0f;
    m_musicDuckScale = 1.0f;
    m_bossAwake = false;
    m_output = nullptr;
    m_narration.bind(nullptr);
    m_barks.bind(nullptr);
}

SoundHandle LevelSoundscape::track(SoundHandle handle) {
    if (m_output != nullptr && handle != kNoSound) {
        std::erase_if(m_voices, [this](SoundHandle voice) { return !m_output->isPlaying(voice); });
        m_voices.push_back(handle);
    }
    return handle;
}

SoundSet* LevelSoundscape::bankOf(std::string_view name, u32& sound) {
    if (name.empty()) {
        return nullptr;
    }
    for (SoundSet* bank : {&m_level, &m_common, &m_ambient}) {
        if (const auto found = bank->find(name); found.has_value()) {
            sound = *found;
            return bank;
        }
    }
    return nullptr;
}

bool LevelSoundscape::isPlaying(SoundHandle handle) const {
    return m_output != nullptr && m_output->isPlaying(handle);
}

SoundHandle LevelSoundscape::playNamed(std::string_view name, f32 volume, SoundHandle after) {
    if (m_output == nullptr) {
        return kNoSound;
    }
    u32 sound = 0;
    SoundSet* bank = bankOf(name, sound);
    if (bank == nullptr) {
        return kNoSound;
    }
    try {
        return track(
            m_output->playAfter(after, bank->sequence(sound), volume, SoundCategory::Effects));
    } catch (const std::exception& e) {
        log::warn("Tower: sound {}: {}", name, e.what());
        return kNoSound;
    }
}

SoundHandle LevelSoundscape::playAt(std::string_view name, const Vec3& position, f32 playerDistance,
                                    f32 volume) {
    const f32 heard = volume * attenuation(playerDistance);
    if (heard <= 0 || m_output == nullptr) {
        return kNoSound;
    }
    const auto handle = playNamed(name, heard);
    if (handle != kNoSound) {
        m_output->setPan(handle, AmbientSounds::panOf(position, m_ear));
    }
    return handle;
}

f32 LevelSoundscape::lengthOf(std::string_view name) {
    for (SoundSet* bank : {&m_level, &m_common, &m_ambient}) {
        if (const auto found = bank->find(name); found.has_value()) {
            return static_cast<f32>(VoiceQueue::lengthOf(bank->sequence(*found)));
        }
    }
    return 0.0f;
}

SoundHandle LevelSoundscape::playFrom(SoundSet& bank, std::string_view name, f32 volume) {
    if (m_output != nullptr) {
        if (const auto found = bank.find(name); found.has_value()) {
            return track(m_output->play(bank.sequence(*found), volume, SoundCategory::Effects));
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
    u32 sound = 0;
    if (m_output != nullptr) {
        if (SoundSet* bank = bankOf(name, sound)) {
            return track(
                m_output->playAfter(after, bank->sequence(sound), 1.0f, SoundCategory::Effects));
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
    return m_narration.room(maxWait);
}

f64 LevelSoundscape::narrationBacklog() const {
    return m_narration.backlog();
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
    u32 sound = 0;
    if (SoundSet* bank = bankOf(name, sound); bank != nullptr) {
        return queue(*bank, sound);
    }
    return kNoSound;
}

SoundHandle LevelSoundscape::queueNarrationFrom(SoundSet& bank, std::string_view name) {
    const auto found = bank.find(name);
    return found.has_value() ? queue(bank, *found) : kNoSound;
}

SoundHandle LevelSoundscape::queue(SoundSet& bank, u32 sound) {
    return track(m_narration.queue(bank.sequence(sound), 1.0f));
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
    m_narration.update(seconds);
    m_barks.update(seconds);
}

bool LevelSoundscape::barkRoom(f32 maxWait) const {
    return m_barks.room(maxWait);
}

SoundHandle LevelSoundscape::bark(SoundSet& bank, std::string_view name, f32 volume, f32 maxWait) {
    const auto found = bank.find(name);
    if (!found.has_value() || !m_barks.room(maxWait)) {
        return kNoSound;
    }
    return track(m_barks.queue(bank.sequence(*found), volume));
}

SoundHandle LevelSoundscape::barkNamed(std::string_view name, f32 volume, f32 maxWait) {
    u32 sound = 0;
    SoundSet* bank = bankOf(name, sound);
    if (bank == nullptr || !m_barks.room(maxWait)) {
        return kNoSound;
    }
    return track(m_barks.queue(bank->sequence(sound), volume));
}

void LevelSoundscape::playPickup() {
    if (m_output != nullptr && m_pickup.has_value()) {
        track(m_output->play(m_common.sequence(*m_pickup), 1.0f, SoundCategory::Effects));
    }
}

f32 LevelSoundscape::attenuation(f32 distance) {
    // sndFxPlay3DAtten: 1.4 - d / 50, held between nothing and the whole.
    const f32 heard = 1.0f - (distance - kAttenuationNear) / (kAttenuationFar - kAttenuationNear);
    return std::clamp(heard, 0.0f, 1.0f);
}

void LevelSoundscape::playFootstep(bool second, Footing footing, f32 distance) {
    const f32 heard = attenuation(distance);
    if (heard <= 0.0f) {
        return;
    }
    const auto kind = kStepKinds[static_cast<usize>(footing)];
    if (const auto found = m_common.find(std::format("S_STEP{}{}", kind, second ? 2 : 1));
        found.has_value() && m_output != nullptr) {
        track(
            m_output->play(m_common.sequence(*found), kStepVolume * heard, SoundCategory::Effects));
    }
}

Footing LevelSoundscape::footingOf(u32 floorFlags, u32 armorFlags, bool inWater) {
    constexpr u32 kMetal = 0x10000;
    constexpr u32 kStairs = 8;
    if (inWater) {
        return Footing::Water;
    }
    if ((armorFlags & kMetal) != 0) {
        return Footing::Metal;
    }
    return (floorFlags & kStairs) != 0 ? Footing::Stair : Footing::Rock;
}

void LevelSoundscape::playEntrance() {
    playNamed(kEntranceSound, kEntranceVolume);
}

SoundHandle LevelSoundscape::playSerpent(const Vec3& position, const Vec3& attention,
                                         f32 playerDistance, const AmbientEar& ear) {
    constexpr f32 kWakeDistance = 40.0f;
    if (glm::distance(position, attention) >= kWakeDistance || m_output == nullptr) {
        return kNoSound;
    }
    const f32 heard = attenuation(playerDistance);
    if (heard <= 0.0f) {
        return kNoSound;
    }
    const auto handle = playNamed("S_SERPENT", kMotionVolume * heard);
    if (handle != kNoSound) {
        m_output->setPan(handle, AmbientSounds::panOf(position, ear));
    }
    return handle;
}

void LevelSoundscape::announceTitle(u32 levelFlags) {
    if ((levelFlags & kStunLevel) != 0) {
        if (narrationRoom(kStunWait)) {
            queueNarration(kStunHint, Narrator::Primary);
        }
    } else if ((levelFlags & kGrabLevel) != 0) {
        if (narrationRoom(kGrabWait)) {
            queueNarration(kGrabHint, Narrator::Primary);
        }
    }
}

SoundHandle LevelSoundscape::announceRune(bool nearby, const Vec3& attention,
                                          const AmbientEar& ear) {
    constexpr f32 kWait = 3.0f;
    constexpr f32 kVolume = 224.0f / 255.0f;
    if (!narrationRoom(kWait) || m_output == nullptr) {
        return kNoSound;
    }
    const auto sound = m_narrator.find(nearby ? "S_RUNENEAR" : "S_UGETCLOSER");
    if (!sound) {
        return kNoSound;
    }
    const auto handle = track(m_narration.queue(m_narrator.sequence(*sound), kVolume));
    if (handle != kNoSound) {
        m_output->setPan(handle, AmbientSounds::panOf(attention, ear));
    }
    return handle;
}

void LevelSoundscape::placeLoop(PlacedLoop& loop, const std::optional<Vec3>& spot,
                                const AmbientEar& ear) {
    if (!spot.has_value() || m_output == nullptr) {
        stopLoop(loop);
        return;
    }
    if (loop.handle == kNoSound || !m_output->isPlaying(loop.handle)) {
        loop.handle = playNamed(loop.name, loop.volume);
    }
    if (loop.handle != kNoSound) {
        m_output->setPan(loop.handle, AmbientSounds::panOf(*spot, ear));
    }
}

void LevelSoundscape::stopLoop(PlacedLoop& loop) {
    stop(loop.handle);
    loop.handle = kNoSound;
}

void LevelSoundscape::updateExitFlame(const std::optional<Vec3>& stander, const AmbientEar& ear) {
    placeLoop(m_exitFlame, stander, ear);
}

void LevelSoundscape::updateHourglass(const std::optional<Vec3>& wearer, const AmbientEar& ear) {
    placeLoop(m_hourglass, wearer, ear);
}

void LevelSoundscape::speakOverScroll(std::string_view name) {
    stopVoice();
    m_voice = playNamed(name);
}

void LevelSoundscape::opening(const TriggerOpening& event) {
    // ProcessItemWobjs tests its signed sound slot before bridge and motion dispatch too.
    if (event.atOnce || event.sound < 0) {
        return;
    }
    if (event.subtype == 20 || event.subtype == 22) {
        if (!m_boss) {
            playNamed(std::format("S_BRID{}{}", event.closed ? "CL" : "OP", m_realm),
                      kMotionVolume);
        }
        return;
    }
    if (event.sound >= kMotionSlot) {
        playMotion(event.sound, event.target, false);
        return;
    }
    if (const SoundHandle handle = playOpening(event.sound, false); handle != kNoSound) {
        m_openings.push_back(Opening{event.target, handle});
    }
}

void LevelSoundscape::settled(const TriggerOpening& event) {
    if (event.subtype == 20 || event.subtype == 22) {
        return;
    }
    if (event.sound >= kMotionSlot) {
        if (!event.atOnce) {
            playMotion(event.sound, event.target, true);
        }
        return;
    }
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

/** AudioWorldObjectMotion (items.c 5236-5246): the trap slot sounds at either edge of the
 * motion; the quake slot once as it starts, for a target flagged for it. */
void LevelSoundscape::playMotion(s32 slot, s32 target, bool settled) {
    if (m_boss) {
        return;
    }
    if (slot == kMotionSlot) {
        playNamed(std::format("{}{}", kTrapMotion, m_realm), kMotionVolume);
        return;
    }
    if (slot != kMotionSlot + 1 || settled || target < 0 ||
        static_cast<usize>(target) >= m_objectFlags.size() ||
        (m_objectFlags[static_cast<usize>(target)] & kMotionFlags) == 0) {
        return;
    }
    if (m_realm == kPyramidRealm) {
        playNamed(kQuakeMotion, kMotionVolume);
    } else if (m_realm == kSkyRealm) {
        playNamed(kClunkMotion, kMotionVolume);
    }
}

} // namespace gdl::game
