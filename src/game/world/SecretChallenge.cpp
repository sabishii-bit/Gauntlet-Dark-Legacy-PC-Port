#include "game/world/SecretChallenge.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace gdl::game {

s32 SecretChallenge::classFor(s32 levelIndex) {
    // GC towerAwardWorldRunes, table at 0x80124DA0: SECRET realm order, not class order.
    // Jackal, Tiger, Falconess, Minotaur, Sumner, Ogre, Hyena, Medusa, Unicorn.
    constexpr std::array<s32, 9> kRewards{10, 11, 9, 8, 16, 12, 15, 14, 13};
    return levelIndex >= 0 && static_cast<usize>(levelIndex) < kRewards.size()
               ? kRewards[static_cast<usize>(levelIndex)]
               : -1;
}

void SecretChallenge::clear() {
    m_state = State::Inactive;
    m_remaining = 0;
    m_duration = 0;
    m_reward = -1;
    m_total = 0;
    m_coins.clear();
}

bool SecretChallenge::begin(s32 levelIndex, s32 seconds, std::span<const usize> coins) {
    clear();
    m_reward = classFor(levelIndex);
    if (m_reward < 0 || seconds <= 0) {
        return false;
    }
    m_duration = static_cast<f32>(seconds) + kClockLead;
    m_remaining = m_duration;
    m_coins.insert(coins.begin(), coins.end());
    m_total = m_coins.size();
    m_state = State::Collecting;
    return true;
}

bool SecretChallenge::collect(usize coin) {
    if (m_state != State::Collecting || m_coins.erase(coin) == 0 || !m_coins.empty()) {
        return false;
    }
    m_state = State::Won;
    m_remaining = kWinExitDelay;
    return true;
}

std::vector<s32> SecretChallenge::step(f32 seconds, bool paused) {
    std::vector<s32> cues;
    if (paused || !std::isfinite(seconds) || seconds <= 0 ||
        (m_state != State::Collecting && m_state != State::Won)) {
        return cues;
    }
    const auto before = static_cast<s32>(m_remaining);
    m_remaining = std::max(0.0f, m_remaining - seconds);
    const auto after = static_cast<s32>(m_remaining);
    if (m_state == State::Collecting) {
        for (s32 second = before - 1; second >= after; --second) {
            cues.push_back(second);
        }
    }
    if (m_remaining <= 0) {
        m_state = State::Returning;
    }
    return cues;
}

u16 SecretChallenge::rewardMask() const {
    constexpr s32 kFirstSecretClass = 8;
    return m_reward >= kFirstSecretClass
               ? static_cast<u16>(u32{1} << (m_reward - kFirstSecretClass))
               : 0;
}

} // namespace gdl::game
