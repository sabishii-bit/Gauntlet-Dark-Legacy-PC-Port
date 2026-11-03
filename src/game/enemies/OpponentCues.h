#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/enemies/OpponentPool.h"

namespace gdl::game {
struct EnemyCue;
struct EnemyFeedback;
struct DeathEvent;
struct GeneratorEvent;

/** Whose voice a cue ended up in. */
enum class CueOwner : u8 { Native, Imported };

/** Where the opponents' sounds go as they happen, one synchronous answer per cue. A cue marked
 * "own scope" opens its emitter's sound scope inside the native answer; the rest are given
 * inside the scope the opponents already hold. */
class OpponentCues {
public:
    /** A launched combatant effect's sound, by its lifetime. */
    struct Shot {
        std::string_view name; ///< borrowed only for the call
        Vec3 position{0.0f};
        u64 instance = 0;
        s32 effect = -1; ///< the authored effect row the sound belongs to
        u32 phase = 0;   ///< which of the effect's sounds it is: launch, impact, end...
        CombatantPool owner = CombatantPool::Critters;
    };
    virtual ~OpponentCues() = default;
    /** A throw going off or a suicide starting its run; own scope. */
    virtual void enemyCue(const EnemyCue& cue) = 0;
    /** What a thrower let fly lands, `level` of 255 loud. */
    virtual void missileLanded(std::string_view sound, f32 level, const Vec3& at) = 0;
    /** A suicide's bang. */
    virtual void suicideBang(std::string_view sound, const Vec3& at) = 0;
    /** Own scope. */
    virtual void shotSound(const Shot& shot) = 0;
    /** One of the swarm hurt or slain. */
    virtual void enemyFeedback(const EnemyFeedback& feedback, std::string_view sound,
                               const Vec3& at) = 0;
    /** An enemy's melee sound. An imported answer also makes the hurt a plain Blow. */
    [[nodiscard]] virtual CueOwner enemyMelee(s32 kind, s32 tier,
                                              const std::optional<std::string>& sound) = 0;
    /** A generator struck; own scope. */
    virtual void generatorFeedback(const GeneratorEvent& event, std::string_view sound) = 0;
    /** A great one's or the boss's sound, `attenuated` by how far the nearest player stands. */
    virtual void combatantSound(std::string_view sound, const Vec3& at, bool attenuated,
                                CombatantPool pool) = 0;
    /** Death's per-kind sound; lessons stay with the caller. */
    virtual void deathCue(const DeathEvent& cue) = 0;
    /** Death's drain contact this tick. An imported answer owns the drain sound. Own scope. */
    [[nodiscard]] virtual CueOwner deathContact(bool contact) = 0;
    /** Death's contact released by a pause or a close. */
    virtual void deathContactReleased() = 0;
};
} // namespace gdl::game
