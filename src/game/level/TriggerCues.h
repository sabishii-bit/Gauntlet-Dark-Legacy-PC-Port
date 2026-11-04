#pragma once

#include <functional>
#include <string_view>

#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"

#include "game/audio/LevelSoundscape.h"
#include "game/level/LevelWorld.h"
#include "game/level/SwitchCutscene.h"

namespace gdl::game {

/**
 * Answers what the level's triggers and rotators reported this frame: the lessons for a party
 * that must share a spot, the scroll telling a refused party what a gate wants, the notes of a
 * target opening and settling, a switch's camera cut, and a turntable's turning and stopping.
 * It keeps only the sound of the turntable turning.
 */
class TriggerCues {
public:
    static constexpr std::string_view kNeedCrystals = "NEEDCRYSTALS";
    static constexpr std::string_view kNeedIcons = "NEEDGARGITEMS";

    /** What the cues ask of the scene. */
    struct Events {
        std::function<bool(s32, usize)> help;                     ///< a lesson for a member
        std::function<bool(std::string_view, usize)> openMessage; ///< a scroll page
        std::function<void()> shake;
        std::function<bool(s32, const Vec3&)> helpAt;
    };

    /** The sounds a realm's turntables turn and stop to (fn_8009D7E4); none elsewhere. */
    struct RotatorSounds {
        std::string_view turning;
        std::string_view stopping;
    };
    static RotatorSounds rotatorSoundsOf(s32 realm);

    void handle(LevelWorld& world, LevelSoundscape& audio, SoundPlayer* sounds,
                SwitchCutscene& cutscene, const Events& events);
    /** Silence a turning platform while gameplay is paused. Its next movement cue
     * restarts the loop; a platform that has settled cannot replay it. */
    void pause(LevelSoundscape& audio);
    /** One aggregate turntable voice, derived after the corrected native tick. */
    static void gatherAudio(const LevelWorld& world, LevelSoundscape& audio);
    SoundHandle turningSound() const { return m_rotatorSound; }

private:
    void handleRotators(LevelWorld& world, LevelSoundscape& audio, SoundPlayer* sounds);

    SoundHandle m_rotatorSound = kNoSound; ///< a turntable turning
};

} // namespace gdl::game
