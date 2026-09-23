#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"

namespace gdl::game {

/** What the wizard says, page by page: the message it is in and the page. */
struct VictoryCaption {
    std::string message;
    usize page = 0;
    usize shown = 0; ///< characters of the page shown so far, the way the original types them
};

/** A line the wizard speaks, from the level's bank. */
struct VictoryVoice {
    std::string sound;
};

/**
 * The end of a boss fight, as the original stages it: a while after the boss falls, the
 * good wizard fades in over the party and, page by page (typed out a character every two
 * ticks, a second's pause between pages), says the boss is beaten and its shard recovered,
 * then how many of the realm's runestones the party has found; then the party is
 * teleported out. The words are the game's own messages; the voices the level's bank's.
 * What is said is looked up by whoever shows it: `update` is told the pages' lengths.
 */
class BossVictory {
public:
    enum class Stage : u8 { None, Waiting, Appearing, Defeat, Runes, Leaving, Gone, Done };

    static constexpr s32 kWaitTicks = 300;     ///< five seconds after the fall
    static constexpr s32 kLongWaitTicks = 600; ///< ten for the demon and the garm
    static constexpr s32 kFadeStep = 4;        ///< of 255, a tick, as the wizard comes
    static constexpr s32 kTicksPerCharacter = 2;
    static constexpr s32 kPagePauseTicks = 60;
    static constexpr s32 kAfterDefeatTicks = 15; ///< half a second before the rune line
    static constexpr s32 kAfterRunesTicks = 30;  ///< a second before he sees them off
    static constexpr s32 kExitTicks = 120;       ///< then the party is taken out
    static constexpr s32 kExitLongTicks = 600;   ///< or later, with gold still lying about
    static constexpr s32 kExitSparkleTicks = 35; ///< the teleport shows this long before

    /** Begins for the boss `kind` in the realm whose levels' letter is `realm`, the party
     * having found `runesFound` of the realm's `runesInRealm` (bit masks of the runestones,
     * the way the level records number them), with `goldLeft` lying about. */
    void begin(s32 kind, char realm, u16 runesInRealm, u16 runesFound, bool goldLeft);
    /** Tells it whether gold still lies about: the wizard waits the long wait for it to be
     * gathered, and no longer than the short one once it is. */
    void setGoldLeft(bool left);
    void clear();

    /** Moves it `ticks` on, told how long each page of the caption up is (none without
     * one). Returns the voices to start. */
    std::vector<VictoryVoice> update(s32 ticks, std::span<const usize> pageLengths);

    Stage stage() const { return m_stage; }
    bool running() const { return m_stage != Stage::None && m_stage != Stage::Done; }
    bool finished() const { return m_stage == Stage::Done; }
    /** The wizard's opacity, none until he comes. */
    f32 wizardAlpha() const { return m_alpha; }
    bool wizardShown() const { return m_stage >= Stage::Appearing && m_stage <= Stage::Gone; }
    /** The caption up, if any. */
    const std::optional<VictoryCaption>& caption() const { return m_caption; }
    /** Whether the teleport's sparkle is on the party. */
    bool sparkling() const { return m_sparkling; }
    /** How the realm's runestones went: 0 none found, 1 some, 2 the one, 3 both. */
    s32 runeQuality() const { return m_quality; }

    /** The defeat message of a boss kind (e.g. `LICH_SPEECH`), empty for one with none. */
    static std::string_view defeatMessageOf(s32 kind);
    /** The rune message for a quality. */
    static std::string_view runeMessageOf(s32 quality);
    /** The voice of a boss's defeat and of its rune line. */
    static std::string defeatVoiceOf(s32 kind, char realm);
    static std::string runeVoiceOf(s32 kind, char realm, s32 quality);
    /** The quality of `found` against a realm's `all`. */
    static s32 qualityOf(u16 all, u16 found);

private:
    void say(std::string_view message, s32 pauseAfter, const std::string& voice,
             std::vector<VictoryVoice>& voices);
    /** Types the caption on; true once every page has been read and the pause is over. */
    bool type(s32 ticks, std::span<const usize> pageLengths);
    void leave();

    Stage m_stage = Stage::None;
    s32 m_kind = -1;
    char m_realm = 'G';
    s32 m_quality = 0;
    bool m_goldLeft = false;
    s32 m_ticksLeft = 0;
    f32 m_alpha = 0.0f;
    std::optional<VictoryCaption> m_caption;
    s32 m_typeTicks = 0;
    s32 m_pauseTicks = 0;
    s32 m_pauseAfter = 0;
    bool m_pageDone = false;
    bool m_pagesOver = false;
    bool m_sparkling = false;
};

} // namespace gdl::game
