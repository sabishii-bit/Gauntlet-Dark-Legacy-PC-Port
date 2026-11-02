#include <algorithm>
#include <array>
#include <string_view>

#include "engine/core/Log.h"
#include "engine/core/Types.h"

#include "game/hud/HelpMessages.h"
#include "game/party/PartyDamage.h"
#include "game/players/PickupVoices.h"
#include "game/scenes/PlayScene.h"

namespace gdl::game {

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
    PlayerHealth::answerTowerPrompts(m_players, inputs, !m_sim.fixedRoster);
}

void PlayScene::hurt(usize index, f32 damage, HurtKind kind, bool directed,
                     const PlayerImpact& impact, bool armorAbsorbed) {
    // damage_player excludes trigger-camera shots even though existing effects
    // and the player's action continue to advance beneath the input lock.
    if (index >= m_players.size() || m_sim.switchCutscene.active()) {
        return;
    }
    if (cheats(m_players[index].actor.player()).god) {
        return; // the developer console's word
    }
    const LevelInfo* level = m_world->level();
    PlayerRuntime& runtime = m_players[index];
    const auto audioScope = m_audio.soundScope({31, 0, 1, static_cast<u8>(runtime.actor.player())},
                                               static_cast<u32>(kind) + 1);
    const PartyDamage::Context context{
        .tower = m_world->isTower(),
        .damageScale = level != nullptr ? level->tuning.damage : 1.0f,
        .bossEncounter =
            (level != nullptr && level->bossType >= 0) ||
            (m_world->imported() != nullptr && m_world->imported()->importedBoss() != nullptr),
        .cutscene = m_sim.switchCutscene.active(),
        .lastRealmLevel =
            m_context.levels != nullptr && m_context.levels->isLastLevel(m_world->ref()),
        .realm = m_world->ref().realmId,
        .playSeconds = m_sim.playSeconds};
    PartyDamage::hurt(m_sim.health, runtime, damage, kind, directed, context,
                      healthEvents(index, runtime), impact,
                      armorAbsorbed ? nullptr : m_classes.stats(runtime.actor.save().character));
}

PlayerHealth::Events PlayScene::healthEvents(usize index, PlayerRuntime& runtime) {
    return {
        .block = [this, index](f32 taken,
                               f32 left) { m_attacks.showBlock(index, taken, left, m_players); },
        .sound = [this](std::string_view sound) { m_audio.playNamed(sound); },
        .cry = [this, index](std::string_view voice) { m_attacks.cry(index, voice, m_players); },
        .named = [this, index](std::string_view line, f32 wait) { sayWithName(index, line, wait); },
        .learnBlock = [this, index] { postHelp(HelpMessages::kLearnBlock, index); },
        .vibrate =
            [this, &runtime](s32 frames) {
                emitOutput(RumbleOutput{runtime.actor.player(), frames, ControlFeedback::Damage});
            }};
}

bool PlayScene::kill(s32 player) {
    if (m_world == nullptr || m_world->isTower()) {
        return false;
    }
    for (usize i = 0; i < m_players.size(); ++i) {
        PlayerRuntime& runtime = m_players[i];
        if (runtime.actor.player() != player || runtime.life != PlayerLife::Standing) {
            continue;
        }
        const auto audioScope = m_audio.soundScope({31, 0, 1, static_cast<u8>(player)},
                                                   static_cast<u32>(HurtKind::Burn) + 1);
        PlayerHealth::fall(runtime, healthEvents(i, runtime));
        return true;
    }
    return false;
}

bool PlayScene::waitInTower(s32 player) {
    for (usize i = 0; i < m_players.size(); ++i) {
        PlayerRuntime& runtime = m_players[i];
        if (runtime.actor.player() != player) {
            continue;
        }
        if (runtime.life != PlayerLife::InTower) {
            runtime.life = PlayerLife::InTower;
            perform(i, PartyMotion::Action::Fallen);
        }
        runtime.towerPrompt = false; // nobody is left at that seat to answer it
        m_sumnerVisit.release(player); // nor to read or put away a scroll it holds
        return true;
    }
    return false;
}

/** Burning floors, rollers and carts hurt whoever is against or on them, at most once a
 * second each (PlayerMotion_FloorFX); the heaviest jolt the mines' carts sound. */
void PlayScene::updateHazardSurfaces(f32 seconds) {
    constexpr f32 kSurfaceGap = 1.0f;
    constexpr s32 kMineRealm = 9;
    constexpr std::string_view kMineCartSound = "S_MINECARPHIT"; ///< AudioWorldHitPlyr's
    for (usize i = 0; i < m_players.size(); ++i) {
        PlayerRuntime& runtime = m_players[i];
        PlayerHealth::advanceSurfaceGap(runtime, seconds);
        if (runtime.surfaceGap > 0.0f || !PlayerHealth::canTakeSurfaceDamage(runtime)) {
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
            const auto audioScope = m_audio.soundScope(
                {44, static_cast<u64>(touch->object), 1, static_cast<u8>(actor.player())}, 1);
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
    if (body == nullptr) {
        return;
    }
    const CharacterSave& save = m_players[index].actor.save();
    const auto audioScope =
        m_audio.soundScope({34, 0, 1, static_cast<u8>(m_players[index].actor.player())}, 1);
    const std::array lines{line};
    m_audio.announce(body->voice(), PickupVoices::nameOf(save.character, save.color),
                     PickupVoices::carriesPojo(save), lines, wait,
                     AnnouncementSubject{save.character, save.color});
}

} // namespace gdl::game
