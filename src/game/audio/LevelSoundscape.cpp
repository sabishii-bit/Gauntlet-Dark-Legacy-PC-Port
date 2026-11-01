#include "game/audio/LevelSoundscape.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "engine/audio/AdsStream.h"
#include "engine/audio/StreamPlaylist.h"
#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/platform/Paths.h"

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
constexpr u32 kAmbientOwner = 10;
constexpr u32 kOpeningOwner = 11;
constexpr u32 kScrollOwner = 16;
constexpr u32 kSharedLoopOwner = 13;

} // namespace

void LevelSoundscape::saveLocal(LocalSnapshot& saved) const {
    saved.m_values = m_sim;
    m_narration.saveLocal(saved.m_narration);
    m_barks.saveLocal(saved.m_barks);
    saved.m_owner = m_lifetime;
    saved.m_epoch = m_localEpoch;
}
bool LevelSoundscape::appendNativeDigest(NativeStateDigest& digest) const {
    digest.string("LevelSoundscape.native.2");
    digest.value(m_sim);
    return m_narration.appendNativeDigest(digest) && m_barks.appendNativeDigest(digest) &&
           digest.good();
}
bool LevelSoundscape::appendLocalDigest(NativeStateDigest& digest,
                                        const LocalSnapshot& saved) const {
    if (!canRestoreLocal(saved)) {
        digest.invalidate();
        return false;
    }
    digest.string("LevelSoundscape.native.2");
    digest.value(saved.m_values);
    return m_narration.appendLocalDigest(digest, saved.m_narration) &&
           m_barks.appendLocalDigest(digest, saved.m_barks) && digest.good();
}
bool LevelSoundscape::canRestoreLocal(const LocalSnapshot& saved) const {
    return saved.m_owner.lock() == m_lifetime && saved.m_epoch == m_localEpoch &&
           std::ranges::all_of(saved.m_values.borrowedBanks,
                               [&](const auto& name) {
                                   return std::ranges::find(m_borrowed, name,
                                                            &BorrowedBank::name) !=
                                          m_borrowed.end();
                               }) &&
           m_narration.canRestoreLocal(saved.m_narration) && m_barks.canRestoreLocal(saved.m_barks);
}
bool LevelSoundscape::restoreLocal(const LocalSnapshot& saved) {
    if (!canRestoreLocal(saved)) {
        return false;
    }
    m_sim = saved.m_values;
    m_narration.restoreLocal(saved.m_narration);
    m_barks.restoreLocal(saved.m_barks);
    m_areas.setPlayerCount(m_sim.players);
    m_ambience.setPlayerCount(m_sim.players);
    return true;
}
bool LevelSoundscape::equalLocal(const LocalSnapshot& left, const LocalSnapshot& right) const {
    return canRestoreLocal(left) && canRestoreLocal(right) && left.m_values == right.m_values &&
           m_narration.equalLocal(left.m_narration, right.m_narration) &&
           m_barks.equalLocal(left.m_barks, right.m_barks);
}

void LevelSoundscape::recordTo(SceneSoundEvents* events) {
    if (events != m_events) {
        requirePresentation("changing the scene sound recorder during a native tick");
        if (events != nullptr) {
            if (events->epoch() == 0 || events->recording()) {
                events->unsupported("attaching scene sound recording requires an open idle epoch");
            }
            // A live opening or scroll has no pre-existing logical birth. Attach
            // before it starts rather than silently lose or restart that lifetime.
            if (!canAttachRecorder()) {
                events->unsupported("attach scene sound recording before owned cues begin");
            }
            if (m_output != nullptr) {
                m_ambience.stop(*m_output);
            }
            stopLoop(m_exitFlame);
            stopLoop(m_hourglass);
            m_voice = kNoSound;
        }
        ++m_localEpoch;
        m_events = events;
        m_narration.recordTo(events);
        m_barks.recordTo(events);
    }
}

bool LevelSoundscape::canAttachRecorder() const {
    return m_events == nullptr && !m_announcements && m_openings.empty() && m_voice == kNoSound &&
           m_narration.backlog() == 0 && m_barks.backlog() == 0;
}

void LevelSoundscape::prepareRecordedFrame(const AmbientEar& ear) {
    m_ear = ear;
    m_ambientDuck.reset();
}

void LevelSoundscape::requirePresentation(std::string_view operation) const {
    if (m_events != nullptr && m_events->recording()) {
        m_events->unsupported(operation);
    }
}

bool LevelSoundscape::hasNamed(std::string_view name) {
    u32 call = 0;
    return bankOf(name, call) != nullptr;
}

SpeechTiming LevelSoundscape::namedTiming(std::string_view name) {
    u32 call = 0;
    if (auto* bank = bankOf(name, call)) {
        return SpeechTiming::of(bank->sequence(call));
    }
    return {};
}

SoundCueToken LevelSoundscape::beginNamedCue(std::string_view name) {
    if (m_events == nullptr) {
        return {};
    }
    u32 call = 0;
    if (auto* bank = bankOf(name, call)) {
        return m_events->beginCue(bank->sequence(call));
    }
    return {};
}

bool LevelSoundscape::desireNamedLoop(const FrameLoopKey& key, std::string_view name,
                                      SceneSoundPlayback playback, f32 volume, f32 pan,
                                      SoundCueToken start) {
    if (m_events == nullptr) {
        return false;
    }
    u32 call = 0;
    if (auto* bank = bankOf(name, call)) {
        m_events->desireLoop(key, bank->sequence(call), playback, volume, pan, start);
        return true;
    }
    return false;
}

void LevelSoundscape::gatherLoops(std::span<const Vec3> listeners, const AmbientEar& ear,
                                  f32 volume, bool ducked, const WorldScene* world,
                                  const std::optional<Vec3>& exit,
                                  const std::optional<Vec3>& hourglass) {
    if (m_events == nullptr) {
        return;
    }
    m_ear = ear;
    const auto requests = m_ambience.requests(
        listeners, volume, ducked ? std::optional<f32>{kDuckedLevel} : std::nullopt, world);
    m_ambientDuck = m_ambience.musicScale(requests);
    for (const auto& request : requests) {
        if (request.loudness <= 0) {
            continue;
        }
        const auto& emitter = m_ambience.emitter(request.emitter);
        m_events->desireLoop({{kAmbientOwner, static_cast<u64>(emitter.instance), 1}, 1, 1},
                             emitter.bank->sequence(emitter.sound),
                             SceneSoundPlayback::RepeatWhileDesired, request.volume,
                             AmbientSounds::panOf(request.position, ear));
    }
    if (!m_sim.cuesEnabled) {
        return;
    }
    const auto owned = [&](const Simulation::OwnedCue& cue, u32 nameSpace) {
        u32 call = 0;
        auto* bank = cue.promotion ? promotionBankOf(cue.name, call) : bankOf(cue.name, call);
        if (bank != nullptr && cue.start) {
            m_events->desireLoop({{nameSpace, static_cast<u64>(std::max(cue.target, 0)), 1}, 1, 1},
                                 bank->sequence(call), SceneSoundPlayback::PlayOnceUntilCancelled,
                                 1.0f, 0, cue.start);
        }
    };
    for (const auto& cue : m_sim.openings) {
        owned(cue, kOpeningOwner);
    }
    if (m_sim.scroll) {
        owned(*m_sim.scroll, kScrollOwner);
    }
    if (exit) {
        desireNamedLoop({{kSharedLoopOwner, 0, 1}, 1, 1}, "S_EXITFLAME",
                        SceneSoundPlayback::RepeatWhileDesired, kLoopVolume,
                        AmbientSounds::panOf(*exit, ear));
    }
    if (hourglass) {
        desireNamedLoop({{kSharedLoopOwner, 0, 1}, 2, 1}, "S_HOURGLASS",
                        SceneSoundPlayback::RepeatWhileDesired, kHourglassVolume,
                        AmbientSounds::panOf(*hourglass, ear));
    }
}

void LevelSoundscape::servicePresentation(f32 seconds) {
    requirePresentation("servicing scene presentation during native capture");
    if (m_ambientDuck) {
        duckAmbientMusic(AmbientSounds::kMusicDuckHold, *m_ambientDuck);
    }
    serviceMusic(seconds);
    if (m_events != nullptr) {
        m_events->servicePlayback();
    }
}

void LevelSoundscape::open(const std::filesystem::path& root, SoundPlayer* output,
                           const LevelAudioInfo* info, char realm, bool boss) {
    close();
    m_sim.cuesEnabled = true;
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

void LevelSoundscape::bindAmbience(const WorldLayout& layout, const WorldScene* world) {
    requirePresentation("rebinding ambient resources during a native tick");
    ++m_localEpoch;
    m_sim.players = 1;
    if (m_output != nullptr) {
        m_ambience.stop(*m_output);
    }
    const std::array<SoundSet*, 2> banks{&m_ambient, &m_level};
    m_ambience.bind(layout, banks, world);
    m_areas.bind(layout, world);
    m_objectFlags.clear();
    m_objectFlags.reserve(layout.objects().size());
    for (const WorldObject& object : layout.objects()) {
        m_objectFlags.push_back(object.flags);
    }
}

void LevelSoundscape::setPlayerCount(s32 count) {
    m_sim.players = count;
    m_ambience.setPlayerCount(count);
    m_areas.setPlayerCount(count);
}

void LevelSoundscape::updateAmbience(std::span<const Vec3> listeners, const AmbientEar& ear,
                                     f32 volume, bool ducked, const WorldScene* world) {
    m_ear = ear;
    if (m_events != nullptr) {
        return; // The root gathers one complete final desire after all replay work.
    }
    if (m_output != nullptr) {
        // Motion loops belong to their still-moving target, not to the menu's
        // clock. Restore only the records that survived a gameplay pause.
        for (auto& opening : m_openings) {
            if (opening.handle == kNoSound) {
                opening.handle = playOpening(opening.slot, false);
            }
        }
        m_ambience.update(*m_output, listeners, ear, volume,
                          ducked ? std::optional<f32>{kDuckedLevel} : std::nullopt, world);
        if (const auto scale = m_ambience.musicScale(); scale.has_value()) {
            duckAmbientMusic(AmbientSounds::kMusicDuckHold, *scale);
        }
    }
}

void LevelSoundscape::pauseAmbience() {
    requirePresentation("pausing ambient mixer voices during a native tick");
    if (m_events != nullptr) {
        m_events->pausePlayback();
        return;
    }
    if (m_output != nullptr) {
        m_ambience.stop(*m_output);
    }
    stopLoop(m_exitFlame);
    stopLoop(m_hourglass);
    // Some opening slots are finite (TOWAMB S_ELVMETL), even when the target
    // keeps moving. Do not replay a cue that ended before the initial pause.
    // A zero handle already belongs to a live cue retained by an earlier pause.
    std::erase_if(m_openings, [this](const Opening& opening) {
        return opening.handle != kNoSound && !isPlaying(opening.handle);
    });
    for (auto& opening : m_openings) {
        stop(opening.handle);
        opening.handle = kNoSound;
    }
}

void LevelSoundscape::holdSounds() {
    requirePresentation("holding mixer voices during a native tick");
    if (m_output != nullptr) {
        m_output->setCategoryPaused(SoundCategory::Effects, true);
    }
}

void LevelSoundscape::releaseSounds() {
    requirePresentation("releasing mixer voices during a native tick");
    if (m_output != nullptr) {
        m_output->setCategoryPaused(SoundCategory::Effects, false);
    }
    if (m_events != nullptr) {
        m_events->resumePlayback();
    }
}

void LevelSoundscape::updateMusicAreas(std::span<const Vec3> listeners, const WorldScene* world) {
    if (const std::optional<MusicCue> cue = m_areas.update(listeners, m_sim.music.area, world);
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
    requirePresentation("starting a music stream during a native tick");
    // A resumed scene may have no soundtrack (native imported maps supply their
    // own). Its gameplay/narration clock still resumes after suspend().
    m_sim.cuesEnabled = true;
    if (assets == nullptr || m_stream.empty()) {
        return;
    }
    // BGMusicStart: the first area at full level, with nothing else asked for.
    m_assets = assets;
    m_musicVolume = volume;
    m_sim.music.area = 0;
    m_sim.music.how = MusicSwitch::AtPartEnd;
    m_sim.music.enabled = true;
    m_sim.duck = 0;
    m_sim.duckScale = 1;
    m_sim.music.start = ++m_sim.music.revision;
    applyMusicRequest();
}

void LevelSoundscape::selectMusicArea(s32 area, MusicSwitch how) {
    m_sim.music.area = area;
    m_sim.music.how = how;
    ++m_sim.music.revision;
}

void LevelSoundscape::applyMusicRequest() {
    const MusicRequest& requested = m_sim.music;
    if (m_musicPlayback.applied == requested) {
        return;
    }
    const bool restart =
        requested.start != m_musicPlayback.applied.start || !m_musicPlayback.applied.enabled;
    m_musicPlayback.applied = requested;
    m_musicPlayback.area = requested.area;
    m_musicPlayback.how = requested.how;
    if (!requested.enabled) {
        stopMusic();
        m_musicOn = false;
        m_musicSilent = false;
        m_playingArea = -1;
        return;
    }
    m_musicOn = true;
    if (restart) {
        m_musicPlayback.level = kFullLevel;
        m_musicPlayback.frames = 0;
        m_musicPlayback.ambientDuck = 0;
        if (m_output != nullptr && m_assets != nullptr) {
            playArea(std::clamp(requested.area, 0, m_streamAreas - 1));
        }
    }
}

void LevelSoundscape::followMusic(const AssetLocator* assets, s32 area, s32 level, f32 volume) {
    requirePresentation("following mixer music during a native tick");
    if (m_output == nullptr || assets == nullptr || area < -1 || area >= m_streamAreas ||
        level < 0 || level > kFullLevel) {
        return;
    }
    m_assets = assets;
    m_musicVolume = volume;
    m_musicPlayback.level = level;
    if (area == -1) {
        stopMusic();
        m_playingArea = -1;
        return;
    }
    if (area != m_playingArea) {
        playArea(area);
    }
    if (m_music != kNoSound) {
        m_output->setVolume(m_music, musicVolume());
    }
}

void LevelSoundscape::bossAwake(bool awake) {
    if (awake && !m_sim.bossAwake) {
        selectMusicArea(1, MusicSwitch::Faded);
    }
    m_sim.bossAwake = awake;
}

void LevelSoundscape::duckMusic(f32 seconds, f32 scale) {
    // AudioClampMusicVol: the scale is kept between a fifth and the whole.
    constexpr f32 kLeastScale = 0.2f;
    m_sim.duck = std::max(seconds, 0.0f);
    m_sim.duckScale = scale < kLeastScale ? kLeastScale : std::min(scale, 1.0f);
}

void LevelSoundscape::duckAmbientMusic(f32 seconds, f32 scale) {
    m_musicPlayback.ambientDuck = std::max(seconds, 0.0f);
    m_musicPlayback.ambientDuckScale = std::clamp(scale, 0.2f, 1.0f);
}

void LevelSoundscape::updateMusic(f32 seconds) {
    // A single-area level ignores alternate selections regardless of whether
    // this machine has an audio device or has consumed a stream callback.
    if (m_streamAreas <= 1 && m_sim.music.area != 0) {
        m_sim.music.area = 0;
        m_sim.music.how = MusicSwitch::AtPartEnd;
    }
    m_sim.duck = std::max(m_sim.duck - seconds, 0.0f);
    if (recording()) {
        return; // Native request normalization above is independent of the mixer.
    }
    serviceMusic(seconds);
}

void LevelSoundscape::serviceMusic(f32 seconds) {
    applyMusicRequest();
    if (m_output == nullptr || m_assets == nullptr || !m_musicOn) {
        return;
    }
    m_musicPlayback.ambientDuck = std::max(m_musicPlayback.ambientDuck - seconds, 0.0f);
    m_musicPlayback.frames += seconds * kMusicRate;
    while (m_musicPlayback.frames >= 1.0f) {
        m_musicPlayback.frames -= 1.0f;
        stepMusic();
    }
}

void LevelSoundscape::stepMusic() {
    setupStreams();
    s32 target = kFullLevel;
    if (m_musicPlayback.how == MusicSwitch::Faded && m_musicPlayback.area != m_playingArea &&
        m_streamAreas > 1) {
        target = m_musicPlayback.level > kFadedLevel ? m_musicPlayback.level - kFadeStep
                                                     : m_musicPlayback.level;
    } else if (m_sim.duck > 0.0f || m_musicPlayback.ambientDuck > 0.0f) {
        const auto scale = m_sim.duck > 0 ? m_sim.duckScale : m_musicPlayback.ambientDuckScale;
        target = static_cast<s32>(static_cast<f32>(kFullLevel) * scale);
    }
    const s32 step = std::clamp(target - m_musicPlayback.level, -kRiseStep, kRiseStep);
    if (step == 0) {
        return;
    }
    m_musicPlayback.level += step;
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
    const s32 area = std::clamp(m_musicPlayback.area, 0, m_streamAreas - 1);
    if (m_music != kNoSound) {
        if (m_musicPlayback.area == m_playingArea) {
            if (m_followArea >= 0) {
                m_playlist->follow({});
                m_followArea = -1;
            }
            return;
        }
        if (m_musicPlayback.how == MusicSwitch::AtPartEnd) {
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
        if (m_musicPlayback.how == MusicSwitch::Faded && m_musicPlayback.level > kFadedLevel) {
            return;
        }
        if (m_streamAreas <= 1) {
            m_musicPlayback.area = 0;
            m_musicPlayback.how = MusicSwitch::AtPartEnd;
            return;
        }
    } else if (m_musicSilent && m_musicPlayback.area == m_playingArea) {
        return;
    }
    playArea(area);
}

void LevelSoundscape::playArea(s32 area) {
    std::vector<std::unique_ptr<StreamSource>> parts = openArea(area);
    stopMusic();
    m_playingArea = area;
    // Playing an area's stream clears what was asked (sMusicSubState goes to nought).
    m_musicPlayback.how = MusicSwitch::AtPartEnd;
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
    return static_cast<f32>(m_musicPlayback.level) / static_cast<f32>(kFullLevel) * m_musicVolume;
}

void LevelSoundscape::stopMusic() {
    stop(m_music);
    m_music = kNoSound;
    m_playlist.reset();
    m_followArea = -1;
    m_followsTaken = 0;
}

void LevelSoundscape::stop(SoundHandle handle) {
    requirePresentation("stopping a physical sound handle during a native tick");
    if (m_output != nullptr && handle != kNoSound) {
        m_output->stop(handle);
    }
}

void LevelSoundscape::stopVoice() {
    if (m_events != nullptr) {
        m_sim.scroll.reset();
        return;
    }
    stop(m_voice);
    m_voice = kNoSound;
}

void LevelSoundscape::stopCues() {
    // Defeat/departure also stops cues within a live scene. Mixer state is not
    // rewind state; only bank/owner changes invalidate logical local history.
    // AudioStopSelect: the stream is let go of until the next start.
    m_sim.music.enabled = false;
    ++m_sim.music.revision;
    m_sim.cuesEnabled = false;
    m_sim.openings.clear();
    m_sim.scroll.reset();
    if (m_events != nullptr) {
        return;
    }
    applyMusicRequest();
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
    if (m_events != nullptr && !recording()) {
        m_events->pausePlayback();
        stopMusic();
    }
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
    requirePresentation("closing scene sound banks during a native tick");
    if (m_events != nullptr) {
        m_events->close(); // Borrowed clips must not outlive the banks below.
        recordTo(nullptr);
    }
    ++m_localEpoch;
    m_sim.players = 1;
    suspend();
    m_announcements = {};
    m_ear = {};
    m_ambientDuck.reset();
    m_ambience.clear();
    m_areas.clear();
    m_objectFlags.clear();
    m_common = SoundSet{};
    m_borrowed.clear();
    m_sim.borrowedBanks.clear();
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
    m_sim.music = {};
    m_sim.duck = 0;
    m_sim.duckScale = 1;
    m_musicPlayback = {};
    m_assets = nullptr;
    m_sim.bossAwake = false;
    m_output = nullptr;
    m_narration.bind(nullptr);
    m_barks.bind(nullptr);
    recordTo(nullptr);
}

usize LevelSoundscape::voicesPlaying() const {
    if (m_output == nullptr) {
        return 0;
    }
    return static_cast<usize>(std::ranges::count_if(
        m_voices, [this](SoundHandle voice) { return m_output->isPlaying(voice); }));
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
    for (const auto& key : m_sim.borrowedBanks) {
        const auto bank = std::ranges::find(m_borrowed, key, &BorrowedBank::name);
        if (bank == m_borrowed.end()) {
            continue;
        }
        if (const auto found = bank->sounds->find(name); found.has_value()) {
            sound = *found;
            return bank->sounds.get();
        }
    }
    return nullptr;
}

bool LevelSoundscape::borrowBank(const std::filesystem::path& directory) {
    // Native WDATA bank names are root-relative. Cache disk resources separately
    // from active lookup order so rollback cannot leak a future bank into a tick.
    const std::string key = paths::genericUtf8(directory.filename());
    auto found = std::ranges::find(m_borrowed, key, &BorrowedBank::name);
    if (found == m_borrowed.end()) {
        auto bank = std::make_unique<SoundSet>();
        if (!bank->load(directory)) {
            return false;
        }
        m_borrowed.push_back({key, directory, std::move(bank)});
    } else if (found->directory != directory) {
        return false; // One logical bank cannot silently bind different roots.
    }
    if (std::ranges::find(m_sim.borrowedBanks, key) == m_sim.borrowedBanks.end()) {
        m_sim.borrowedBanks.push_back(key);
    }
    return true;
}

bool LevelSoundscape::isPlaying(SoundHandle handle) const {
    requirePresentation("polling mixer playback from a native tick");
    return m_output != nullptr && m_output->isPlaying(handle);
}

SoundHandle LevelSoundscape::playSequence(const SoundSequence& sequence, f32 volume, f32 pan,
                                          SoundHandle after) {
    if (m_events != nullptr) {
        if (after != kNoSound) {
            m_events->unsupported("physical sound predecessor requires a logical speech cue");
        }
        m_events->emit(sequence, volume, pan);
        return kNoSound;
    }
    if (m_output == nullptr) {
        return kNoSound;
    }
    const auto handle = m_output->playAfter(after, sequence, volume, SoundCategory::Effects);
    if (handle != kNoSound && pan != 0) {
        m_output->setPan(handle, pan);
    }
    return handle;
}

SoundHandle LevelSoundscape::playNamed(std::string_view name, f32 volume, SoundHandle after) {
    return playNamedPanned(name, volume, 0, after);
}

SoundHandle LevelSoundscape::playNamedPanned(std::string_view name, f32 volume, f32 pan,
                                             SoundHandle after) {
    if (m_output == nullptr && m_events == nullptr) {
        return kNoSound;
    }
    u32 sound = 0;
    SoundSet* bank = bankOf(name, sound);
    if (bank == nullptr) {
        return kNoSound;
    }
    try {
        return track(playSequence(bank->sequence(sound), volume, pan, after));
    } catch (const SceneSoundEvents::Unsupported&) {
        throw;
    } catch (const std::exception& e) {
        log::warn("Tower: sound {}: {}", name, e.what());
        return kNoSound;
    }
}

SoundHandle LevelSoundscape::playAt(std::string_view name, const Vec3& position, f32 playerDistance,
                                    f32 volume) {
    const f32 heard = volume * attenuation(playerDistance);
    if (heard <= 0 || (m_output == nullptr && m_events == nullptr)) {
        return kNoSound;
    }
    return playNamedPanned(name, heard, AmbientSounds::panOf(position, m_ear));
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
    if (m_output != nullptr || m_events != nullptr) {
        if (const auto found = bank.find(name); found.has_value()) {
            return track(playSequence(bank.sequence(*found), volume));
        }
    }
    return kNoSound;
}

SoundHandle LevelSoundscape::playFromCall(SoundSet& bank, u32 call, f32 volume) {
    if (m_output == nullptr && m_events == nullptr) {
        return kNoSound;
    }
    return track(playSequence(bank.sequence(call), volume));
}

SoundHandle LevelSoundscape::playPromotion(std::string_view name, SoundHandle after) {
    u32 sound = 0;
    if (m_output != nullptr || m_events != nullptr) {
        if (SoundSet* bank = promotionBankOf(name, sound)) {
            return track(playSequence(bank->sequence(sound), 1, 0, after));
        }
    }
    return after;
}

SoundHandle LevelSoundscape::narrate(std::string_view name, Narrator which, SoundHandle after) {
    if (m_output == nullptr && m_events == nullptr) {
        return kNoSound;
    }
    u32 sound = 0;
    if (SoundSet* bank = narrationBankOf(name, which, sound)) {
        return track(playSequence(bank->sequence(sound), 1, 0, after));
    }
    return kNoSound;
}

SoundSet* LevelSoundscape::narrationBankOf(std::string_view name, Narrator which, u32& sound) {
    for (SoundSet* bank : {&m_narrator, &m_narratorSecond}) {
        if (const auto found = bank->find(name); found.has_value()) {
            sound = *found;
            return bank;
        }
        if (which == Narrator::Primary) {
            break;
        }
    }
    return nullptr;
}

SoundSet* LevelSoundscape::promotionBankOf(std::string_view name, u32& sound) {
    if (const auto found = m_promotions.find(name)) {
        sound = *found;
        return &m_promotions;
    }
    if (SoundSet* bank = bankOf(name, sound)) {
        return bank;
    }
    // The shared level-99 speech is in the main narrator bank, not WIZTOWER.
    return narrationBankOf(name, Narrator::Primary, sound);
}

LevelSoundscape::Speech LevelSoundscape::speech(SoundSet& bank, u32 sound, const Speech& after) {
    const auto sequence = bank.sequence(sound);
    Speech result = after;
    const auto timing = SpeechTiming::of(sequence);
    result.timing.append(timing);
    if (m_events != nullptr) {
        if (!timing.playing()) {
            return after;
        }
        if (after.handle != kNoSound && !after.cue) {
            m_events->unsupported("speech chain lacks its logical predecessor cue");
        }
        result.cue = m_events->emit(sequence, 1, 0, after.cue);
        result.handle = kNoSound;
        return result;
    }
    if (m_output != nullptr) {
        // Optional playback must not erase a decoded line's gameplay duration.
        // Keep source decoding outside this catch: an invalid bank is not an
        // audio-device failure, and retains the caller's existing error policy.
        try {
            const auto handle =
                m_output->playAfter(after.handle, sequence, 1.0f, SoundCategory::Effects);
            if (handle != kNoSound) {
                result.handle = handle;
            }
            track(handle);
        } catch (const std::exception& e) {
            log::warn("Tower: speech output: {}", e.what());
        }
    }
    return result;
}

LevelSoundscape::Speech LevelSoundscape::speechNamed(std::string_view name, const Speech& after) {
    u32 sound = 0;
    SoundSet* bank = bankOf(name, sound);
    if (bank != nullptr) {
        try {
            return speech(*bank, sound, after);
        } catch (const SceneSoundEvents::Unsupported&) {
            throw;
        } catch (const std::exception& e) {
            log::warn("Tower: sound {}: {}", name, e.what());
        }
    }
    return after;
}

LevelSoundscape::Speech LevelSoundscape::speechFrom(SoundSet& bank, std::string_view name) {
    const auto found = bank.find(name);
    return found ? speech(bank, *found, {}) : Speech{};
}

LevelSoundscape::Speech LevelSoundscape::speechPromotion(std::string_view name,
                                                         const Speech& after) {
    u32 sound = 0;
    SoundSet* bank = promotionBankOf(name, sound);
    return bank != nullptr ? speech(*bank, sound, after) : after;
}

LevelSoundscape::Speech LevelSoundscape::speechNarration(std::string_view name, Narrator which) {
    u32 sound = 0;
    SoundSet* bank = narrationBankOf(name, which, sound);
    return bank != nullptr ? speech(*bank, sound, {}) : Speech{};
}

bool LevelSoundscape::narrationRoom(f32 maxWait) const {
    return m_narration.room(maxWait);
}

f64 LevelSoundscape::narrationBacklog() const {
    return m_narration.backlog();
}

void LevelSoundscape::bindAnnouncements(AnnouncementResolver resolver) {
    requirePresentation("binding an announcement adapter during a native tick");
    if (m_events != nullptr || m_narration.backlog() != 0) {
        throw std::logic_error("announcement adapters require an idle offline soundscape");
    }
    ++m_localEpoch;
    m_announcements = std::move(resolver);
}

SoundHandle LevelSoundscape::queueNarration(std::string_view name, Narrator which,
                                            std::optional<AnnouncementSubject> subject) {
    if (m_announcements && !m_narration.held()) {
        const std::array lines{name};
        if (const auto sequence = m_announcements(lines, subject)) {
            return track(m_narration.queue(*sequence, 1));
        }
    }
    return queueNarrationFallback(name, which);
}

SoundHandle LevelSoundscape::queueNarrationFallback(std::string_view name, Narrator which) {
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
                               std::span<const std::string_view> lines, f32 maxWait,
                               std::optional<AnnouncementSubject> subject) {
    if (!narrationRoom(maxWait)) {
        return false;
    }
    if (m_announcements && subject) {
        subject->pojo = pojo;
        if (const auto sequence = m_announcements(lines, subject)) {
            track(m_narration.queue(*sequence, 1));
            return true;
        }
    }
    if (pojo) {
        queueNarrationFallback(kPojoName, Narrator::Either);
    } else {
        queueNarrationFrom(characterBank, name);
    }
    for (const std::string_view line : lines) {
        queueNarrationFallback(line, Narrator::Either);
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
    if ((m_output != nullptr || m_events != nullptr) && m_pickup.has_value()) {
        track(playSequence(m_common.sequence(*m_pickup)));
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
        found.has_value() && (m_output != nullptr || m_events != nullptr)) {
        track(playSequence(m_common.sequence(*found), kStepVolume * heard));
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
    if (glm::distance(position, attention) >= kWakeDistance ||
        (m_output == nullptr && m_events == nullptr)) {
        return kNoSound;
    }
    const f32 heard = attenuation(playerDistance);
    if (heard <= 0.0f) {
        return kNoSound;
    }
    return playNamedPanned("S_SERPENT", kMotionVolume * heard, AmbientSounds::panOf(position, ear));
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
    if (!narrationRoom(kWait)) {
        return kNoSound;
    }
    const auto sound = m_narrator.find(nearby ? "S_RUNENEAR" : "S_UGETCLOSER");
    if (!sound) {
        return kNoSound;
    }
    return track(m_narration.queue(m_narrator.sequence(*sound), kVolume,
                                   AmbientSounds::panOf(attention, ear)));
}

void LevelSoundscape::placeLoop(PlacedLoop& loop, const std::optional<Vec3>& spot,
                                const AmbientEar& ear) {
    requirePresentation("placed sounds require a desired-loop event boundary");
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
    if (m_events != nullptr) {
        return; // Final root gather supplies the portal's current stander.
    }
    placeLoop(m_exitFlame, stander, ear);
}

void LevelSoundscape::updateHourglass(const std::optional<Vec3>& wearer, const AmbientEar& ear) {
    if (m_events != nullptr) {
        return;
    }
    placeLoop(m_hourglass, wearer, ear);
}

void LevelSoundscape::speakOverScroll(std::string_view name) {
    if (m_events != nullptr) {
        auto scope = soundScope({kScrollOwner, 0, 1}, 1);
        u32 call = 0;
        auto* bank = bankOf(name, call);
        const bool promotion = bank == nullptr;
        if (promotion) {
            bank = promotionBankOf(name, call);
        }
        m_sim.scroll.reset();
        if (bank != nullptr) {
            const auto cue = m_events->beginCue(bank->sequence(call));
            m_sim.scroll = Simulation::OwnedCue{std::string{name}, cue, -1, promotion};
        }
        return;
    }
    stopVoice();
    m_voice = playNamed(name);
    if (m_voice == kNoSound) {
        m_voice = playPromotion(name);
    }
}

void LevelSoundscape::opening(const TriggerOpening& event) {
    auto scope = soundScope({kOpeningOwner, static_cast<u64>(std::max(event.target, 0)), 1}, 1);
    // ProcessItemWobjs tests its signed sound slot before bridge and motion dispatch too.
    if (event.atOnce || event.sound < 0) {
        return;
    }
    if (LevelTrigger::bridgeSubtype(event.subtype)) {
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
    if (m_events != nullptr) {
        const auto name = openingName(event.sound, false);
        const auto start = beginNamedCue(name);
        if (start) {
            std::erase_if(m_sim.openings,
                          [&](const auto& cue) { return cue.target == event.target; });
            m_sim.openings.push_back({name, start, event.target});
        }
        return;
    }
    if (const SoundHandle handle = playOpening(event.sound, false); handle != kNoSound) {
        m_openings.push_back(Opening{event.target, event.sound, handle});
    }
}

void LevelSoundscape::settled(const TriggerOpening& event) {
    auto scope = soundScope({kOpeningOwner, static_cast<u64>(std::max(event.target, 0)), 1}, 2);
    if (LevelTrigger::bridgeSubtype(event.subtype)) {
        return;
    }
    if (event.sound >= kMotionSlot) {
        if (!event.atOnce) {
            playMotion(event.sound, event.target, true);
        }
        return;
    }
    if (m_events != nullptr) {
        std::erase_if(m_sim.openings, [&](const auto& cue) { return cue.target == event.target; });
        playOpening(event.sound, true);
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
    return playNamed(openingName(slot, settled));
}

std::string LevelSoundscape::openingName(s32 slot, bool settled) const {
    if (slot < 0 || static_cast<usize>(slot) >= kOpeningMaterials.size()) {
        return {};
    }
    // GC 800a1614 constructs the sixth pair without a realm suffix, and
    // selects the B-suffixed elevator names in boss arenas.
    const auto material = kOpeningMaterials[static_cast<usize>(slot)];
    if (material.starts_with('*')) {
        return std::format("S_{}{}", material.substr(1), settled ? "STOP" : "ROTATE");
    }
    return std::format("S_ELV{}{}{}{}", material, settled ? "STP" : "", m_realm, m_boss ? "B" : "");
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
