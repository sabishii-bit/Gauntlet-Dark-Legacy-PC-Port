#pragma once

#include <span>
#include <unordered_set>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::game {

/** A shared, timed coin hunt. Only the coins present at entry count toward its reward. */
class SecretChallenge {
public:
    enum class State : u8 { Inactive, Collecting, Won, Returning };
    // GC fn_800553B4 starts at wavetime + .99; fn_80054E78 cues integer crossings.
    static constexpr f32 kClockLead = 0.99f;
    static constexpr f32 kWinExitDelay = 1.0f;

    bool begin(s32 levelIndex, s32 seconds, std::span<const usize> coins);
    void clear();
    /** Returns true exactly once, when the final distinct coin awards the class. */
    bool collect(usize coin);
    /** Advances only active play; returns crossed countdown seconds for audio. */
    std::vector<s32> step(f32 seconds, bool paused = false);
    State state() const { return m_state; }
    f32 remaining() const { return m_remaining; }
    f32 duration() const { return m_duration; }
    usize coinsLeft() const { return m_coins.size(); }
    usize totalCoins() const { return m_total; }
    s32 rewardClass() const { return m_reward; }
    u16 rewardMask() const;
    static s32 classFor(s32 levelIndex);

private:
    State m_state = State::Inactive;
    f32 m_remaining = 0;
    f32 m_duration = 0;
    s32 m_reward = -1;
    usize m_total = 0;
    std::unordered_set<usize> m_coins;
};

} // namespace gdl::game
