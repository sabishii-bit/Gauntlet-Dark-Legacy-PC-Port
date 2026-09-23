#include "game/screens/BossVictory.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>

namespace gdl::game {

namespace {

constexpr std::int32_t kDemon = 42;
constexpr std::int32_t kDemonUnderworld = 43;
constexpr std::int32_t kGarm = 44;

} // namespace

std::string_view BossVictory::defeatMessageOf(std::int32_t kind) {
    switch (kind) {
    case 34: return "DRAGON_SPEECH";
    case 35: return "CHIMERA_SPEECH";
    case 36: return "DJINN_SPEECH";
    case 37: return "DRIDER_SPEECH";
    case 38: return "PBOSS_SPEECH";
    case 39: return "YETI_SPEECH";
    case 40: return "WRAITH_SPEECH";
    case 41: return "LICH_SPEECH";
    case kDemon: return "SKORNE1_SPEECH";
    case kDemonUnderworld: return "SKORNE2_SPEECH";
    case kGarm: return "GARM2_SPEECH";
    default: return "";
    }
}

std::string_view BossVictory::runeMessageOf(std::int32_t quality) {
    switch (quality) {
    case 0: return "RUNE_PHRASE0";
    case 1: return "RUNE_PHRASE1";
    case 2: return "RUNE_PHRASE1B";
    default: return "RUNE_PHRASE2";
    }
}

std::string BossVictory::defeatVoiceOf(std::int32_t kind, char realm) {
    switch (kind) {
    case kDemon: return "S_E2VOXA";
    case kDemonUnderworld: return "S_ENDVOX";
    case kGarm: return "S_GRMDESTVOX";
    default: return std::format("S_DEFEATVOX{}", realm);
    }
}

std::string BossVictory::runeVoiceOf(std::int32_t kind, char realm, std::int32_t quality) {
    if (kind >= kDemon) {
        return kind == kDemon ? "S_E2VOXB" : std::string{};
    }
    // None found, some, the one (of one), both.
    std::int32_t which = 1;
    if (quality <= 0) {
        which = 0;
    } else if (quality == 3) {
        which = 2;
    }
    return std::format("S_RUNEVOX{}{}", which, realm);
}

std::int32_t BossVictory::qualityOf(std::uint16_t all, std::uint16_t found) {
    if (all == 0) {
        return 0;
    }
    if ((all & found) == all) {
        return std::popcount(all) > 1 ? 3 : 2;
    }
    return (all & found) != 0 ? 1 : 0;
}

void BossVictory::begin(std::int32_t kind, char realm, std::uint16_t runesInRealm,
                        std::uint16_t runesFound, bool goldLeft) {
    clear();
    m_stage = Stage::Waiting;
    m_kind = kind;
    m_realm = realm;
    m_quality = qualityOf(runesInRealm, runesFound);
    m_goldLeft = goldLeft;
    m_ticksLeft = kind == kDemon || kind == kGarm ? kLongWaitTicks : kWaitTicks;
}

void BossVictory::clear() {
    m_stage = Stage::None;
    m_kind = -1;
    m_realm = 'G';
    m_quality = 0;
    m_goldLeft = false;
    m_ticksLeft = 0;
    m_alpha = 0.0f;
    m_caption.reset();
    m_typeTicks = 0;
    m_pauseTicks = 0;
    m_pauseAfter = 0;
    m_pageDone = false;
    m_pagesOver = false;
    m_sparkling = false;
}

void BossVictory::say(std::string_view message, std::int32_t pauseAfter, const std::string& voice,
                      std::vector<VictoryVoice>& voices) {
    m_caption = VictoryCaption{std::string(message), 0, 0};
    m_typeTicks = 0;
    m_pauseTicks = 0;
    m_pauseAfter = pauseAfter;
    m_pageDone = false;
    m_pagesOver = false;
    if (!voice.empty()) {
        voices.push_back(VictoryVoice{voice});
    }
}

/** A character every two ticks; a page done waits a second, then the next; past the last
 * page the caption comes down and the pause after is waited out. */
bool BossVictory::type(std::int32_t ticks, std::span<const std::size_t> pageLengths) {
    if (m_pagesOver) {
        m_pauseTicks += ticks;
        return m_pauseTicks >= m_pauseAfter;
    }
    if (!m_caption.has_value() || m_caption->page >= pageLengths.size()) {
        m_caption.reset();
        m_pagesOver = true;
        m_pauseTicks = ticks;
        return m_pauseTicks >= m_pauseAfter;
    }
    const std::size_t length = pageLengths[m_caption->page];
    if (!m_pageDone) {
        m_typeTicks += ticks;
        m_caption->shown =
            std::min(static_cast<std::size_t>(m_typeTicks / kTicksPerCharacter), length);
        m_pageDone = m_caption->shown >= length;
        m_pauseTicks = 0;
        return false;
    }
    m_pauseTicks += ticks;
    if (m_pauseTicks >= kPagePauseTicks) {
        ++m_caption->page;
        m_caption->shown = 0;
        m_typeTicks = 0;
        m_pauseTicks = 0;
        m_pageDone = false;
        if (m_caption->page >= pageLengths.size()) {
            m_caption.reset();
            m_pagesOver = true;
            return m_pauseAfter <= 0;
        }
    }
    return false;
}

void BossVictory::leave() {
    m_stage = Stage::Leaving;
    m_ticksLeft = m_goldLeft ? kExitLongTicks : kExitTicks;
}

void BossVictory::setGoldLeft(bool left) {
    m_goldLeft = left;
    if (!left && m_stage == Stage::Leaving) {
        m_ticksLeft = std::min(m_ticksLeft, kExitTicks);
    }
}

std::vector<VictoryVoice> BossVictory::update(std::int32_t ticks,
                                              std::span<const std::size_t> pageLengths) {
    std::vector<VictoryVoice> voices;
    if (!running() || ticks <= 0) {
        return voices;
    }
    switch (m_stage) {
    case Stage::Waiting:
        m_ticksLeft -= ticks;
        if (m_ticksLeft <= 0) {
            m_stage = Stage::Appearing;
            m_alpha = 0.0f;
        }
        break;
    case Stage::Appearing:
        m_alpha = std::min(m_alpha + static_cast<float>(kFadeStep * ticks) / 255.0f, 1.0f);
        if (m_alpha >= 1.0f) {
            m_stage = Stage::Defeat;
            say(defeatMessageOf(m_kind), kAfterDefeatTicks, defeatVoiceOf(m_kind, m_realm), voices);
        }
        break;
    case Stage::Defeat:
        if (type(ticks, pageLengths)) {
            const std::string voice = runeVoiceOf(m_kind, m_realm, m_quality);
            if (m_kind >= kDemon) {
                // The demon's and the garm's have no rune line of the realm's.
                leave();
            } else {
                m_stage = Stage::Runes;
                say(runeMessageOf(m_quality), kAfterRunesTicks, voice, voices);
            }
        }
        break;
    case Stage::Runes:
        if (type(ticks, pageLengths)) {
            leave();
        }
        break;
    case Stage::Leaving:
        m_ticksLeft -= ticks;
        if (m_ticksLeft <= kExitSparkleTicks && !m_sparkling) {
            m_sparkling = true;
            m_stage = Stage::Gone;
        }
        break;
    case Stage::Gone:
        m_ticksLeft -= ticks;
        if (m_ticksLeft <= 0) {
            m_stage = Stage::Done;
        }
        break;
    case Stage::None:
    case Stage::Done: break;
    }
    return voices;
}

} // namespace gdl::game
