#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/core/Strings.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"
#include "engine/platform/Paths.h"

#include "expansions/dreamcast/enemies/LegendsDreamcastEnemies.h"
#include "expansions/n64/enemies/LegendsEnemyAssets.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "game/enemies/Generators.h"

namespace gdl::game {
std::unique_ptr<ItemArchive> LegendsLevel::loadEnemyAssets(s32 kind) const {
    if (!hasEnemyAssets()) {
        return {};
    }
    const auto root = m_directory.parent_path().parent_path();
    if (platform() == LegendsPlatform::Dreamcast) {
        std::vector<std::string> warnings;
        auto archive =
            loadLegendsDreamcastEnemyAssets(root, kind, warnings, m_dreamcastEnemyRoster);
        for (const auto& warning : warnings)
            log::warn("Dreamcast enemy {}: {}", kind, warning);
        return archive;
    }
    const auto program = AssetLocator(root).find("_program/game.bin");
    if (!program) {
        throw FileError("Native enemies require the N64 program tables");
    }
    std::vector<std::string> warnings;
    auto archive = loadLegendsEnemyAssets(root, kind, readFile(*program), warnings);
    for (const auto& warning : warnings) {
        log::warn("N64 enemy {}: {}", kind, warning);
    }
    if (!archive) {
        log::warn("N64 enemy {} has no supported native artwork", kind);
    }
    return archive;
}

bool LegendsLevel::playEnemyFeedback(const EnemyFeedback& feedback, f32 health) {
    if (platform() == LegendsPlatform::Dreamcast) {
        // AudioSetupEnemyStreams 8c0df380 and the hurt/death dispatchers
        // 8c0dfa80/8c0df9e0 use the same stem/tier/hit-count naming contract as
        // GDL. Supply DC's selected ENMY roster, never GDL's empty donor roster.
        return playOpponentSound(feedback.sound(m_dreamcastEnemyRoster), feedback.position);
    }
    if (platform() != LegendsPlatform::N64) {
        return false;
    }
    if (m_enemySounds) {
        if (const auto cue = m_enemySounds->feedback(feedback, health)) {
            m_opponentSounds.push_back(*cue);
        }
    }
    return true;
}

bool LegendsLevel::playEnemyCue(const EnemyCue& cue) {
    if (platform() == LegendsPlatform::Dreamcast) {
        // 8c0dfb80/8c0dfbc0 emit arrow/bolt; 8c0dfcc0 emits pursuit yell.
        m_opponentSounds.push_back(cue.kind == EnemyCue::Kind::Arrow  ? 0x29U
                                   : cue.kind == EnemyCue::Kind::Bolt ? 0x2bU
                                                                      : 0x30U);
        return true;
    }
    if (platform() != LegendsPlatform::N64) {
        return false;
    }
    // e005cabc plays 0x14 only for an arrow; bombs have no launch cue.
    // e005e550 plays 0x94 once when a native suicide starts pursuing its target.
    if (cue.kind == EnemyCue::Kind::Arrow) {
        m_opponentSounds.push_back(0x14);
    } else if (cue.kind == EnemyCue::Kind::Yell) {
        m_opponentSounds.push_back(0x94);
    }
    return true;
}

bool LegendsLevel::playEnemyMelee(s32 kind, s32 tier) {
    if (platform() == LegendsPlatform::Dreamcast) {
        if (const auto sound = EnemyFeedback::meleeSound(kind, tier, m_dreamcastEnemyRoster))
            return playOpponentSound(*sound, Vec3{0});
        return false; // Ordinary weapon strikes retain the player's hurt cue.
    }
    if (platform() != LegendsPlatform::N64) {
        return false;
    }
    if (m_enemySounds) {
        if (const auto cue = m_enemySounds->melee(kind, tier)) {
            m_opponentSounds.push_back(*cue);
        }
    }
    return true;
}

std::optional<bool> LegendsLevel::suicideUsesGas() const {
    if (platform() != LegendsPlatform::N64) {
        return {};
    }
    // e005ec28 tests the native realm, not a GDL level-name prefix. Realm 7's
    // e006c360/e006c468 entries are town1..town5; its green-barrel suicides
    // call poison e0074eec instead of fire e007496c.
    const auto map = toLowerAscii(paths::utf8(m_directory.filename()));
    return map == "town1" || map == "town2" || map == "town3" || map == "town4" || map == "town5";
}

bool LegendsLevel::playDeathCue(const DeathEvent& cue) {
    if (platform() != LegendsPlatform::N64) {
        return false;
    }
    // e005c270 plays this Death cue when magic expels him; e005d734 also
    // plays it when his hundred drain updates finish. Native Death has no
    // separate stone-statue shatter sound to substitute for the GDL one.
    if (cue.kind == DeathEvent::Kind::Killed || cue.kind == DeathEvent::Kind::Exhausted) {
        m_opponentSounds.push_back(0x2b);
    }
    return true;
}

bool LegendsLevel::setDeathContact(bool active) {
    if (platform() != LegendsPlatform::N64) {
        return false;
    }
    m_deathDrainContact = active;
    if (!active && m_soundOutput) {
        m_soundOutput->stop(m_deathDrainSound);
        m_deathDrainSound = kNoSound;
    }
    return true;
}

bool LegendsLevel::playGeneratorFeedback(const GeneratorEvent& event) {
    if (platform() == LegendsPlatform::Dreamcast) {
        if (const auto token = dreamcastGeneratorSound(m_dreamcastRealm, event.destroyed))
            m_opponentSounds.push_back(*token);
        return true;
    }
    if (platform() != LegendsPlatform::N64) {
        return false;
    }
    // e0066ee8 uses these non-spatial cues for both regular and mixed broods.
    const auto name = paths::utf8(m_directory.filename());
    const bool portal = name == "boss1" || name == "hell1";
    m_opponentSounds.push_back(event.destroyed ? (portal ? 0x3fU : 0x3cU)
                                               : (portal ? 0x3eU : 0x15U));
    return true;
}
} // namespace gdl::game
