#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/enemies/OpponentCues.h"
#include "game/enemies/OpponentPool.h"

namespace gdl::game {
class ImportedLevel;

/** Lets an imported level take the opponents' cues it owns, the rest falling to `native`.
 * Built per use from the world's current imported level (null on a native level); neither is
 * kept past that use. */
class ImportedOpponentCues final : public OpponentCues {
public:
    ImportedOpponentCues(ImportedLevel* level, OpponentCues& native);
    void enemyCue(const EnemyCue& cue) override;
    /** Only the lobber's bomb is the level's to sound. */
    void missileLanded(std::string_view sound, f32 level, const Vec3& at) override;
    void suicideBang(std::string_view sound, const Vec3& at) override;
    /** Only the boss's shots are the level's to sound. */
    void shotSound(const Shot& shot) override;
    void enemyFeedback(const EnemyFeedback& feedback, std::string_view sound,
                       const Vec3& at) override;
    /** The level is asked whether or not the melee has a sound of its own. */
    CueOwner enemyMelee(s32 kind, s32 tier, const std::optional<std::string>& sound) override;
    /** The level is asked even when the generator has no sound of its own. */
    void generatorFeedback(const GeneratorEvent& event, std::string_view sound) override;
    /** Only the boss's sounds, and only named ones, are the level's to sound. */
    void combatantSound(std::string_view sound, const Vec3& at, bool attenuated,
                        CombatantPool pool) override;
    void deathCue(const DeathEvent& cue) override;
    CueOwner deathContact(bool contact) override;
    void deathContactReleased() override;

private:
    ImportedLevel* m_level;
    OpponentCues& m_native;
};
} // namespace gdl::game
