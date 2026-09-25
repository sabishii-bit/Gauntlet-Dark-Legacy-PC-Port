#include <algorithm>
#include <format>

#include "engine/core/Log.h"

#include "game/players/ItemPickup.h"
#include "game/screens/PlayScene.h"

namespace gdl::game {

void PlayScene::beginChallenge() {
    if (!m_world->ref().isSecret() || m_context.levels == nullptr) {
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
    constexpr u32 kTimedLevel = 4;
    if (level == nullptr || (level->flags & kTimedLevel) == 0 ||
        !m_challenge.begin(index, level->timeLimit, coins)) {
        log::warn("Secret challenge {} has no time limit; refresh WDATA with gdlunpack",
                  m_world->ref().name);
        return;
    }
    m_challengeHud.bind(*m_device, m_world->powerups());
    m_hud.setCountTextures(&m_world->items().textures);
}

void PlayScene::collectChallengeCoin(usize item) {
    const usize before = m_challenge.coinsLeft();
    const bool won = m_challenge.collect(item);
    if (before == m_challenge.coinsLeft()) {
        return;
    }
    const std::string icon = m_challenge.rewardClass() == kSumnerClass
                                 ? "16_SUM"
                                 : std::format("16_{}COIN", classCode(m_challenge.rewardClass()));
    for (auto& player : m_players) {
        if (player.life == PlayerLife::Standing) {
            m_hud.pickups().showCount(
                player.actor.player(), icon,
                static_cast<s32>(m_challenge.totalCoins() - m_challenge.coinsLeft()),
                static_cast<s32>(m_challenge.totalCoins()));
            if (won) {
                player.actor.save().classUnlock |= m_challenge.rewardMask();
            }
        }
    }
    if (!won) {
        return;
    }
    openMessage("ALLCOINS", 0);
    m_audio.speakOverScroll("S_SECRETCHAR");
}

bool PlayScene::updateChallenge(f32 seconds) {
    for (const s32 second : m_challenge.step(seconds)) {
        if (second == 0) {
            m_audio.playNamed("S_SECRETCLOCKEN");
        } else {
            m_audio.playNamed(second % 2 == 0 ? "S_SECRETCLOCK1" : "S_SECRETCLOCK2");
        }
        constexpr s32 kTimeWarning = 8;
        constexpr s32 kCountdown = 5;
        if (second == kTimeWarning) {
            m_audio.narrate("S_TIMEISRUNNING", LevelSoundscape::Narrator::Primary);
        } else if (second > 0 && second <= kCountdown) {
            m_audio.narrate(std::format("S_COUNT{}", second), LevelSoundscape::Narrator::Primary);
        }
    }
    m_challengeHud.step(seconds);
    return m_challenge.state() == SecretChallenge::State::Returning;
}

void PlayScene::suspendForChallenge() {
    m_audio.suspend();
}

void PlayScene::resumeFromChallenge(std::span<const PartyMember> party) {
    usize standing = 0;
    for (auto& runtime : m_players) {
        const auto member = std::ranges::find(party, runtime.actor.player(), &PartyMember::player);
        if (member == party.end()) {
            continue;
        }
        runtime.actor.save() = member->save;
        runtime.slot = member->slot;
        runtime.helpHeard = member->helpHeard;
        if (runtime.life == PlayerLife::Standing) {
            runtime.actor.place(m_secretReturnPosition +
                                Vec3{static_cast<f32>(standing++) * kSpawnSpacing, 0, 0});
            runtime.actor.settle(m_world->collision());
        }
    }
    m_audio.startMusic(m_context.assets,
                       m_world->level() != nullptr ? m_world->level()->musicVolume : 1.0f);
    m_secretTravel = false;
    m_transition.cover();
    m_transition.clearAway();
}

} // namespace gdl::game
