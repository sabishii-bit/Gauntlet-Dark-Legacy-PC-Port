#include "game/bosses/BossVictory.h"

#include <algorithm>
#include <bit>
#include <format>

#include "engine/core/Types.h"

#include "game/bosses/BossTraits.h"
#include "game/level/TowerAccess.h"

namespace gdl::game {

std::string_view BossVictory::defeatMessageOf(s32 kind) {
    const BossTraits* traits = bossTraitsOf(kind);
    return traits != nullptr ? traits->defeat.speech : std::string_view{};
}

std::string_view BossVictory::runeMessageOf(s32 quality) {
    switch (quality) {
    case 0: return "RUNE_PHRASE0";
    case 1: return "RUNE_PHRASE1";
    case 2: return "RUNE_PHRASE1B";
    default: return "RUNE_PHRASE2";
    }
}

std::string BossVictory::defeatVoiceOf(s32 kind, char realm) {
    const BossTraits* traits = bossTraitsOf(kind);
    if (traits != nullptr && !traits->defeat.defeatVoice.empty()) {
        return std::string{traits->defeat.defeatVoice};
    }
    return std::format("S_DEFEATVOX{}", realm);
}

std::string BossVictory::runeVoiceOf(s32 kind, char realm, s32 quality) {
    const RuneVoices lines = orderedBossTraitsOf(kind).defeat.runeVoices;
    switch (lines) {
    case RuneVoices::Temple: return "S_E2VOXB";
    case RuneVoices::None: return {};
    case RuneVoices::SomeIsAll:
    case RuneVoices::OwnAll: break;
    }
    // None found, some, or all of them (the one or both). The first four realms' guardians
    // say the same for some as for all; the last four have a line of their own for all
    // (AudioGoodWizard's table).
    s32 which = 1;
    if (quality <= 0) {
        which = 0;
    } else if (quality >= 2 && lines == RuneVoices::OwnAll) {
        which = 2;
    }
    return std::format("S_RUNEVOX{}{}", which, realm);
}

s32 BossVictory::qualityOf(u16 all, u16 found) {
    if (all == 0) {
        return 0;
    }
    if ((all & found) == all) {
        return std::popcount(all) > 1 ? 3 : 2;
    }
    return (all & found) != 0 ? 1 : 0;
}

void BossVictory::begin(s32 kind, char realm, u16 runesInRealm, u16 runesFound, bool goldLeft) {
    clear();
    m_stage = Stage::Waiting;
    m_kind = kind;
    m_realm = realm;
    m_quality = qualityOf(runesInRealm, runesFound);
    m_allTempleRunes =
        (runesFound & TowerAccess::kUnderworldRunes) == TowerAccess::kUnderworldRunes;
    m_goldLeft = goldLeft;
    const BossTraits* traits = bossTraitsOf(kind);
    m_ticksLeft = traits != nullptr && traits->defeat.longWizardWait ? kLongWaitTicks : kWaitTicks;
}

void BossVictory::clear() {
    m_stage = Stage::None;
    m_kind = -1;
    m_realm = 'G';
    m_quality = 0;
    m_goldLeft = false;
    m_allTempleRunes = false;
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

void BossVictory::say(std::string_view message, s32 pauseAfter, const std::string& voice,
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

/** CaptionTextSub spends 1.75 text units per character, two more at commas/periods,
 * five for a tab and thirty for a carriage return; line feeds cost nothing.
 * A page completes only when the scan has budget left to reach its terminator. */
bool BossVictory::type(s32 ticks, std::span<const std::string> pages) {
    if (m_pagesOver) {
        m_pauseTicks += ticks;
        return m_pauseTicks >= m_pauseAfter;
    }
    if (!m_caption.has_value() || m_caption->page >= pages.size()) {
        m_caption.reset();
        m_pagesOver = true;
        m_pauseTicks = ticks;
        return m_pauseTicks >= m_pauseAfter;
    }
    const std::string& page = pages[m_caption->page];
    if (!m_pageDone) {
        m_typeTicks += ticks;
        const s32 textUnits = m_typeTicks / kTicksPerTextUnit; // whole units, like frame >> 1
        f32 remaining = static_cast<f32>(textUnits);
        usize shown = 0;
        while (remaining > 0 && shown < page.size()) {
            switch (page[shown++]) {
            case '\r': remaining -= 30; break;
            case '\n': break;
            case '\t': remaining -= 5; break;
            case ',':
            case '.': remaining -= 2; [[fallthrough]];
            default: remaining -= 1.75f; break;
            }
        }
        m_caption->shown = shown;
        m_pageDone = shown == page.size() && remaining > 0;
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
        if (m_caption->page >= pages.size()) {
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

std::vector<VictoryVoice> BossVictory::update(s32 ticks, std::span<const std::string> pages,
                                              bool voicePlaying) {
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
        m_alpha = std::min(m_alpha + static_cast<f32>(kFadeStep * ticks) / 255.0f, 1.0f);
        if (m_alpha >= 1.0f) {
            m_stage = Stage::Defeat;
            // Temple Skorne's voice is one recording spanning both caption sets.
            // DoGoodWizard selects its variant before the first page, using the
            // party's union of the first twelve runes (hide_rune_stones).
            const BossTraits* traits = bossTraitsOf(m_kind);
            const bool templeRunes =
                traits != nullptr && traits->defeat.followUp == VictoryFollowUp::TempleRunes;
            const std::string voice =
                templeRunes && m_allTempleRunes ? "S_E2VOXB" : defeatVoiceOf(m_kind, m_realm);
            say(defeatMessageOf(m_kind), kAfterDefeatTicks, voice, voices);
        }
        break;
    case Stage::Defeat:
        if (type(ticks, pages)) {
            const std::string voice = runeVoiceOf(m_kind, m_realm, m_quality);
            switch (orderedBossTraitsOf(m_kind).defeat.followUp) {
            case VictoryFollowUp::TempleRunes:
                m_stage = Stage::Runes;
                say(m_allTempleRunes ? "SKORNE1_RUNE_YES" : "SKORNE1_RUNE_NO", kAfterRunesTicks, {},
                    voices);
                break;
            case VictoryFollowUp::Leaves:
                // Underworld Skorne and Garm have no rune follow-up.
                leave();
                break;
            case VictoryFollowUp::Realm:
                m_stage = Stage::Runes;
                say(runeMessageOf(m_quality), kAfterRunesTicks, voice, voices);
                break;
            }
        }
        break;
    case Stage::Runes:
        if (type(ticks, pages)) {
            leave();
        }
        break;
    case Stage::Leaving:
        if (voicePlaying) {
            break;
        }
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
