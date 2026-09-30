#pragma once

#include <functional>
#include <string_view>

#include "engine/audio/SoundPlayer.h"
#include "engine/core/Types.h"

#include "game/screens/SwitchCutscene.h"
#include "game/world/LevelSoundscape.h"
#include "game/world/LevelWorld.h"

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
    /** A gargoyle gate's trigger id less this is its tier. */
    static constexpr s32 kIconTierBase = 101;

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

private:
    void handleRotators(LevelWorld& world, LevelSoundscape& audio, SoundPlayer* sounds);

    SoundHandle m_rotatorSound = kNoSound; ///< a turntable turning
};

} // namespace gdl::game
