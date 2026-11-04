#include "game/level/TriggerCues.h"

#include <vector>

#include "engine/core/Types.h"

#include "game/hud/HelpMessages.h"
#include "game/level/LevelTriggers.h"

namespace gdl::game {

namespace {

constexpr s32 kCastleRealm = 1;
constexpr s32 kMineRealm = 9;

} // namespace

void TriggerCues::pause(LevelSoundscape& audio) {
    if (audio.recordingSounds()) {
        return; // The output dispatcher owns playback; native rotators keep their state.
    }
    audio.stop(m_rotatorSound);
    m_rotatorSound = kNoSound;
}

TriggerCues::RotatorSounds TriggerCues::rotatorSoundsOf(s32 realm) {
    if (realm == kCastleRealm) {
        return {"S_ROCKROTATE", "S_ROCKSTOP"};
    }
    if (realm == kMineRealm) {
        return {"S_METLROTATE", "S_METLROTATESTO"};
    }
    return {};
}

void TriggerCues::handleRotators(LevelWorld& world, LevelSoundscape& audio, SoundPlayer* sounds) {
    const RotatorSounds named = rotatorSoundsOf(world.ref().realmId);
    for (const RotatorCue& cue : world.takeRotatorCues()) {
        if (audio.recording()) {
            if (cue.kind == RotatorCue::Kind::Stopped && !named.stopping.empty() &&
                cue.rotator < world.rotators().size()) {
                const auto& rotator = world.rotators().rotator(cue.rotator);
                const auto scope = audio.soundScope(
                    {12, static_cast<u64>(rotator.instance), 1, FrameEventEmitter::kNoSeat}, 1);
                audio.playNamed(named.stopping);
            }
            continue;
        }
        if (named.turning.empty() || sounds == nullptr) {
            continue;
        }
        if (cue.kind == RotatorCue::Kind::Turning) {
            if (!sounds->isPlaying(m_rotatorSound)) {
                m_rotatorSound = audio.playNamed(named.turning);
            }
        } else {
            audio.stop(m_rotatorSound);
            m_rotatorSound = kNoSound;
            audio.playNamed(named.stopping);
        }
    }
}

void TriggerCues::gatherAudio(const LevelWorld& world, LevelSoundscape& audio) {
    if (!audio.recording()) {
        return;
    }
    const auto named = rotatorSoundsOf(world.ref().realmId);
    if (named.turning.empty()) {
        return;
    }
    for (usize index = 0; index < world.rotators().size(); ++index) {
        const auto& rotator = world.rotators().rotator(index);
        if (rotator.subtype == Rotators::kTurnedByPad && rotator.started && !rotator.done) {
            // Retail owns one aggregate handle, not one voice per rotating pad.
            audio.desireNamedLoop({{12, 0, 1, FrameEventEmitter::kNoSeat}, 1, 1}, named.turning,
                                  SceneSoundPlayback::RepeatWhileDesired);
            return;
        }
    }
}

void TriggerCues::handle(LevelWorld& world, LevelSoundscape& audio, SoundPlayer* sounds,
                         SwitchCutscene& cutscene, const Events& events) {
    handleRotators(world, audio, sounds);
    for (const TriggerLesson& lesson : world.takeTriggerLessons()) {
        if (lesson.party >= 0 && events.help) {
            events.help(lesson.platform ? HelpMessages::kAllOnPlatform
                                        : HelpMessages::kAllOnTrigger,
                        static_cast<usize>(lesson.party));
        }
    }
    // One scroll at a time: the frame's first refusal.
    if (const std::vector<TriggerRefusal> refusals = world.takeTriggerRefusals();
        !refusals.empty() && events.openMessage) {
        const TriggerRefusal& refusal = refusals.front();
        if (refusal.crystals) {
            events.openMessage(kNeedCrystals, static_cast<usize>(refusal.id));
        } else if (const s32 tier = refusal.id - LevelTrigger::kIconTierBase; tier >= 0) {
            events.openMessage(kNeedIcons, static_cast<usize>(tier));
        }
    }
    for (const TriggerOpening& opening : world.takeTriggerOpenings()) {
        audio.opening(opening);
    }
    for (const TriggerOpening& settled : world.takeTriggerSettled()) {
        audio.settled(settled);
    }
    for (const TriggerCameraCue& cue : world.takeTriggerCameraCues()) {
        if (cue.shakes && events.shake) {
            events.shake();
        }
        cutscene.begin(cue, world.layout(), world.isTower());
    }
    if (!cutscene.active() && !world.isTower() && events.helpAt) {
        const auto& triggers = world.triggers();
        for (usize i = 0; i < triggers.size(); ++i) {
            if (triggers.trigger(i).movementLesson &&
                events.helpAt(HelpMessages::kTrapsMove, triggers.trigger(i).spot)) {
                break;
            }
        }
    }
}

} // namespace gdl::game
