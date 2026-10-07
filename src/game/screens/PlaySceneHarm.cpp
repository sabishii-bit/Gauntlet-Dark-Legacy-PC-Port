#include <algorithm>
#include <array>
#include <string_view>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

#include "game/players/PickupVoices.h"
#include "game/screens/HelpMessages.h"
#include "game/screens/PlayScene.h"

namespace gdl::game {

namespace {

constexpr f32 kBlockLessonAfter = 60.0f; ///< the guard is taught only after a minute

} // namespace

void PlayScene::hurtPlayer(s32 player, f32 damage, HurtKind kind, bool directed) {
    for (usize i = 0; i < m_players.size(); ++i) {
        if (m_players[i].actor.player() == player) {
            hurt(i, damage, kind, directed);
        }
    }
}

void PlayScene::harm(s32 player, f32 damage, HurtKind kind) {
    for (usize i = 0; i < m_players.size(); ++i) {
        if (m_players[i].actor.player() == player) {
            hurt(i, damage, kind);
        }
    }
}

/** A fallen player asked to wait in the tower or quit answers with their own buttons
 * (player.c 2452): accept waits, back leaves the game (abort_player), out of the party. */
void PlayScene::answerTowerPrompts(const Inputs& inputs) {
    for (PlayerRuntime& runtime : m_players) {
        const auto player = static_cast<usize>(runtime.actor.player());
        if (!runtime.towerPrompt || runtime.life != PlayerLife::InTower ||
            player >= inputs.size()) {
            continue;
        }
        if (inputs[player].menu.back) {
            runtime.towerPrompt = false;
            runtime.departed = true;
            log::info("Player {} has left the game", player + 1);
        } else if (inputs[player].menu.select) {
            runtime.towerPrompt = false;
        }
    }
}

void PlayScene::hurt(usize index, f32 damage, HurtKind kind, bool directed,
                     const PlayerImpact& impact) {
    // damage_player excludes trigger-camera shots even though existing effects
    // and the player's action continue to advance beneath the input lock.
    if (index >= m_players.size() || m_switchCutscene.active()) {
        return;
    }
    const LevelInfo* level = m_world->level();
    PlayerRuntime& runtime = m_players[index];
    const bool stood = runtime.life == PlayerLife::Standing;
    m_health.hurt(
        runtime, damage, kind, directed, m_world->isTower(),
        level != nullptr ? level->tuning.damage : 1.0f,
        {.block = [this, index](f32 taken,
                                f32 left) { m_attacks.showBlock(index, taken, left, m_players); },
         .sound = [this](std::string_view sound) { m_audio.playNamed(sound); },
         .cry = [this, index](std::string_view voice) { m_attacks.cry(index, voice, m_players); },
         .named = [this, index](std::string_view line,
                                f32 wait) { sayWithName(index, line, wait); },
         .learnBlock =
             [this, index] {
                 if (m_playSeconds > kBlockLessonAfter) {
                     postHelp(HelpMessages::kLearnBlock, index);
                 }
             },
         .vibrate =
             [this, &runtime](s32 frames) {
                 if (m_context.vibrate) {
                     m_context.vibrate(runtime.actor.player(), frames, ControlFeedback::Damage);
                 }
             }},
        impact, level != nullptr && level->bossType >= 0,
        m_classes.stats(runtime.actor.save().character));
    // A death on a realm's last level is a try at its boss, kept in the entry save the fallen
    // go on with (inactivate_player, then playerGiveGargItem into the checkpoint copy).
    if (stood && runtime.life == PlayerLife::Dying && m_context.levels != nullptr &&
        m_context.levels->isLastLevel(m_world->ref())) {
        runtime.entrySave.progress().levels.recordBossDeath(m_world->ref().realmId);
    }
}

/** Burning floors, rollers and carts hurt whoever is against or on them, at most once a
 * second each (PlayerMotion_FloorFX); the heaviest jolt the mines' carts sound. */
void PlayScene::updateHazardSurfaces(f32 seconds) {
    constexpr f32 kSurfaceGap = 1.0f;
    constexpr s32 kMineRealm = 9;
    constexpr std::string_view kMineCartSound = "S_MINECARPHIT"; ///< AudioWorldHitPlyr's
    for (usize i = 0; i < m_players.size(); ++i) {
        PlayerRuntime& runtime = m_players[i];
        runtime.surfaceGap = std::max(runtime.surfaceGap - seconds, 0.0f);
        if (runtime.life != PlayerLife::Standing || runtime.capture.held() ||
            runtime.surfaceGap > 0.0f) {
            continue;
        }
        const PlayerActor& actor = runtime.actor;
        const auto touch =
            m_world->hazards().touching(m_world->collision(), actor.position(), actor.radius(),
                                        actor.height(), actor.wallContacts());
        if (!touch) {
            continue;
        }
        runtime.surfaceGap = kSurfaceGap;
        if (touch->harm.jolts && m_world->ref().realmId == kMineRealm) {
            m_audio.playNamed(kMineCartSound);
        }
        hurt(i, touch->harm.damage, HurtKind::Blow, true,
             PlayerImpact{touch->harm.impact, touch->away});
    }
}

/** The narrator names the character ("Red Warrior", from the class's own bank) and says
 * `line` after: what the original's announcements by name do. */
void PlayScene::sayWithName(usize index, std::string_view line, f32 wait) {
    PlayerFigure* body = index < m_players.size() ? m_players[index].figure.get() : nullptr;
    if (body == nullptr || m_context.sounds == nullptr) {
        return;
    }
    const CharacterSave& save = m_players[index].actor.save();
    const std::array lines{line};
    m_audio.announce(body->voice(), PickupVoices::nameOf(save.character, save.color),
                     PickupVoices::carriesPojo(save), lines, wait);
}

} // namespace gdl::game
