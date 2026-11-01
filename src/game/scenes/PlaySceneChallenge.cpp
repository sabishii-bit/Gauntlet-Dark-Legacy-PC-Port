#include <algorithm>
#include <format>

#include "engine/core/Log.h"

#include "game/party/PlayerPowerups.h"
#include "game/players/ItemPickup.h"
#include "game/scenes/PlayScene.h"

namespace gdl::game {

std::optional<ChallengeHud::Look> PlayScene::hourglassLook() const {
    if (const auto stopTime = m_hud.hourglassLook(m_players)) {
        return stopTime;
    }
    if (m_sim.challenge.state() == SecretChallenge::State::Inactive) {
        return std::nullopt;
    }
    return m_challengeHud.look(m_sim.challenge.remaining(), m_sim.challenge.duration(),
                               !spawning() && !m_messages.active());
}

void PlayScene::beginChallenge() {
    if (!m_world->ref().isSecret()) {
        return;
    }
    PlayerPowerups::restrictToChallenge(m_players);
    if (m_context.levels == nullptr) {
        return;
    }
    s32 index = -1;
    for (const auto& realm : m_context.levels->realms()) {
        if (realm.id == LevelRef::kSecretRealm) {
            const auto found = std::ranges::find(realm.levels, m_world->ref().name);
            if (found != realm.levels.end()) {
                index = static_cast<s32>(found - realm.levels.begin());
            }
        }
    }
    std::vector<usize> coins;
    const auto& items = m_world->placedItems();
    for (usize i = 0; i < items.size(); ++i) {
        const auto& item = items.item(i);
        if (item.visible && !item.taken && item.subtype == static_cast<s32>(ItemKind::Gold)) {
            coins.push_back(i);
        }
    }
    const auto* level = m_world->level();
    if (level == nullptr || (level->flags & LevelInfo::kTimed) == 0 ||
        !m_sim.challenge.begin(index, level->timeLimit, coins)) {
        log::warn("Secret challenge {} has no time limit; refresh WDATA with gdlunpack",
                  m_world->ref().name);
        return;
    }
    // The eight-second warning belongs to SELECT; the numbered voices are VOICE2.
    m_audio.borrowBank(m_context.unpackedRoot / "audio/SELECT");
    m_challengeHud.bind(*m_device, m_world->powerups());
    m_hud.setCountTextures(&m_world->items().textures);
}

void PlayScene::collectChallengeCoin(usize item) {
    const usize before = m_sim.challenge.coinsLeft();
    const bool won = m_sim.challenge.collect(item);
    if (before == m_sim.challenge.coinsLeft()) {
        return;
    }
    const std::string icon =
        m_sim.challenge.rewardClass() == kSumnerClass
            ? "16_SUM"
            : std::format("16_{}COIN", classCode(m_sim.challenge.rewardClass()));
    for (auto& player : m_players) {
        if (player.life == PlayerLife::Standing) {
            m_hud.pickups().showCount(
                player.actor.player(), icon,
                static_cast<s32>(m_sim.challenge.totalCoins() - m_sim.challenge.coinsLeft()),
                static_cast<s32>(m_sim.challenge.totalCoins()));
            if (won) {
                player.actor.save().classUnlock |= m_sim.challenge.rewardMask();
            }
        }
    }
    if (!won) {
        return;
    }
    emitOutput(UnlockClassesOutput{m_sim.challenge.rewardMask()});
    openMessage("ALLCOINS", 0);
    m_audio.speakOverScroll("S_SECRETCHAR");
}

bool PlayScene::updateChallenge(f32 seconds) {
    if (m_world->ref().isSecret()) {
        PlayerPowerups::restrictToChallenge(m_players);
    }
    for (const s32 second : m_sim.challenge.step(seconds)) {
        if (second == 0) {
            m_audio.playNamed("S_SECRETCLOCKEN");
        } else {
            m_audio.playNamed(second % 2 == 0 ? "S_SECRETCLOCK1" : "S_SECRETCLOCK2");
        }
        constexpr s32 kTimeWarning = 8;
        constexpr s32 kCountdown = 5;
        if (second == kTimeWarning) {
            m_audio.playNamed("S_TIMEISRUNNING");
        } else if (second > 0 && second <= kCountdown) {
            m_audio.narrate(std::format("S_COUNT{}", second));
        }
    }
    m_challengeHud.step(seconds);
    return m_sim.challenge.state() == SecretChallenge::State::Returning;
}

void PlayScene::suspendForChallenge() {
    // Travel is a confirmed application boundary, not a speculative tick. The
    // suspended scene's old history must not survive into its resumed epoch.
    endRecordedAudio();
    m_audio.suspend();
    // do_exit80077D38 calls the common warp cue even for the secret portal's
    // zero-tick departure. Start it after silencing the parent level, whose
    // sound banks remain alive while the challenge runs.
    if (m_sim.secretTravel) {
        m_audio.playNamed(PortalDeparture::kSound, PortalDeparture::kVolume);
    }
}

void PlayScene::suspendForImportedLevel(bool preserveImportedSpeech) {
    if (!preserveImportedSpeech && m_world && m_world->imported()) {
        m_world->imported()->stopSounds();
    }
    // The parked scene keeps its actors and fixtures, but its mixer handles must
    // not leak into the new scene. This is an offline application boundary.
    endRecordedAudio();
    pauseGameplaySounds();
    m_audio.suspend();
    m_audio.releaseSounds();
}

void PlayScene::resumeAfterImportedLevel() {
    if (!m_open || m_world == nullptr) {
        return;
    }
    m_audio.startMusic(m_context.assets,
                       m_world->level() != nullptr ? m_world->level()->musicVolume : 1.0f);
    m_audio.releaseSounds();
}

bool PlayScene::recoverImportedDeparture() {
    auto* imported = m_world ? m_world->imported() : nullptr;
    if (!m_open || !m_sim.leaving || !imported || !imported->cancelDeparture()) {
        return false;
    }
    // Destination loading is an offline application boundary. Keep this
    // scene's actors/rewards, remove its departure pose, and restore input.
    // The imported owner must also prevent immediate re-admission in place.
    m_sim.leaving = false;
    m_sim.destination = {};
    m_transition.clearAway();
    resumeAfterImportedLevel();
    return true;
}

void PlayScene::resumeFromChallenge(std::span<const PartyMember> party) {
    m_sim.consoleTravel.reset();
    usize standing = 0;
    for (auto& runtime : m_players) {
        const auto member = std::ranges::find(party, runtime.actor.player(), &PartyMember::player);
        if (member == party.end()) {
            continue;
        }
        runtime.actor.save() = member->save;
        runtime.slot = member->slot;
        runtime.helpHeard = member->helpHeard;
        if (member->resultsCheckpoint) {
            // The returned history includes the suspended stage's kills already.
            // Resume that same run without replacing its death/abort checkpoint.
            runtime.resultsCheckpoint = member->resultsCheckpoint;
            runtime.levelKills = 0;
        }
        if (runtime.life == PlayerLife::Standing) {
            runtime.actor.place(m_sim.secretReturnPosition +
                                Vec3{static_cast<f32>(standing++) * kSpawnSpacing, 0, 0});
            runtime.actor.settle(m_world->collision());
        }
    }
    m_audio.startMusic(m_context.assets,
                       m_world->level() != nullptr ? m_world->level()->musicVolume : 1.0f);
    m_sim.secretTravel = false;
    // A hard cut back to the level, as the original's sub-level return is.
}

} // namespace gdl::game
