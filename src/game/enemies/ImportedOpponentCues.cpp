#include "game/enemies/ImportedOpponentCues.h"

#include <optional>
#include <string>
#include <string_view>

#include "engine/core/Types.h"
#include "engine/math/Math.h"

#include "game/enemies/EncounterLevel.h"
#include "game/enemies/EnemyFeedback.h"
#include "game/enemies/OpponentCues.h"
#include "game/enemies/OpponentPool.h"
#include "game/level/ImportedLevel.h"

namespace gdl::game {
namespace {
constexpr std::string_view kLobberBombSound = "S_LOBBER_BOMB";
} // namespace

ImportedOpponentCues::ImportedOpponentCues(ImportedLevel* level, OpponentCues& native,
                                           EncounterLevel& nativeEncounter)
    : m_level(level), m_native(native), m_nativeEncounter(nativeEncounter) {}

void ImportedOpponentCues::enemyCue(const EnemyCue& cue) {
    if (m_level == nullptr || !m_level->playEnemyCue(cue)) {
        m_native.enemyCue(cue);
    }
}

void ImportedOpponentCues::missileLanded(std::string_view sound, f32 level, const Vec3& at) {
    if (sound != kLobberBombSound || m_level == nullptr || !m_level->playOpponentSound(sound, at)) {
        m_native.missileLanded(sound, level, at);
    }
}

void ImportedOpponentCues::suicideBang(std::string_view sound, const Vec3& at) {
    if (m_level == nullptr || !m_level->playOpponentSound(sound, at)) {
        m_native.suicideBang(sound, at);
    }
}

void ImportedOpponentCues::shotSound(const Shot& shot) {
    if (shot.owner != CombatantPool::Boss || m_level == nullptr ||
        !m_level->playOpponentSound(shot.name, shot.position)) {
        m_native.shotSound(shot);
    }
}

void ImportedOpponentCues::enemyFeedback(const EnemyFeedback& feedback, std::string_view sound,
                                         const Vec3& at) {
    if (m_level == nullptr || !m_level->playEnemyFeedback(feedback, feedback.remainingHealth)) {
        m_native.enemyFeedback(feedback, sound, at);
    }
}

CueOwner ImportedOpponentCues::enemyMelee(s32 kind, s32 tier,
                                          const std::optional<std::string>& sound) {
    if (m_level != nullptr && m_level->playEnemyMelee(kind, tier)) {
        return CueOwner::Imported;
    }
    return m_native.enemyMelee(kind, tier, sound);
}

void ImportedOpponentCues::generatorFeedback(const GeneratorEvent& event, std::string_view sound) {
    if (m_level == nullptr || !m_level->playGeneratorFeedback(event)) {
        m_native.generatorFeedback(event, sound);
    }
}

void ImportedOpponentCues::combatantSound(std::string_view sound, const Vec3& at, bool attenuated,
                                          CombatantPool pool) {
    if (pool != CombatantPool::Boss || sound.empty() || m_level == nullptr ||
        !m_level->playOpponentSound(sound, at)) {
        m_native.combatantSound(sound, at, attenuated, pool);
    }
}

void ImportedOpponentCues::deathCue(const DeathEvent& cue) {
    if (m_level == nullptr || !m_level->playDeathCue(cue)) {
        m_native.deathCue(cue);
    }
}

CueOwner ImportedOpponentCues::deathContact(bool contact) {
    if (m_level != nullptr && m_level->setDeathContact(contact)) {
        return CueOwner::Imported;
    }
    return m_native.deathContact(contact);
}

void ImportedOpponentCues::deathContactReleased() {
    if (m_level == nullptr || !m_level->setDeathContact(false)) {
        m_native.deathContactReleased();
    }
}

void ImportedOpponentCues::awoke(const Vec3& at) {
    if (m_level != nullptr) {
        m_level->bossIntro(at);
    } else {
        m_nativeEncounter.awoke(at);
    }
}

void ImportedOpponentCues::deathPlayed(const Vec3& at) {
    if (m_level != nullptr) {
        m_level->bossDeathPlayed(at);
    } else {
        m_nativeEncounter.deathPlayed(at);
    }
}

void ImportedOpponentCues::spendLegend(CharacterSave& bearer, s32 realm) {
    if (m_level != nullptr) {
        m_level->consumeBossLegend(bearer);
    } else {
        m_nativeEncounter.spendLegend(bearer, realm);
    }
}
} // namespace gdl::game
