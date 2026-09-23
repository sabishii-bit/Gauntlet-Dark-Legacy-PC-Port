#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gdl::game {

/** What the wizard says, page by page: the message it is in and the page. */
struct VictoryCaption {
    std::string message;
    std::size_t page = 0;
    std::size_t shown = 0; ///< characters of the page shown so far, the way the original types them
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
    enum class Stage : std::uint8_t {
        None,
        Waiting,
        Appearing,
        Defeat,
        Runes,
        Leaving,
        Gone,
        Done
    };

    static constexpr int kWaitTicks = 300;     ///< five seconds after the fall
    static constexpr int kLongWaitTicks = 600; ///< ten for the demon and the garm
    static constexpr int kFadeStep = 4;        ///< of 255, a tick, as the wizard comes
    static constexpr int kTicksPerCharacter = 2;
    static constexpr int kPagePauseTicks = 60;
    static constexpr int kAfterDefeatTicks = 15; ///< half a second before the rune line
    static constexpr int kAfterRunesTicks = 30;  ///< a second before he sees them off
    static constexpr int kExitTicks = 120;       ///< then the party is taken out
    static constexpr int kExitLongTicks = 600;   ///< or later, with gold still lying about
    static constexpr int kExitSparkleTicks = 35; ///< the teleport shows this long before

    /** Begins for the boss `kind` in the realm whose levels' letter is `realm`, the party
     * having found `runesFound` of the realm's `runesInRealm` (bit masks of the runestones,
     * the way the level records number them), with `goldLeft` lying about. */
    void begin(int kind, char realm, std::uint16_t runesInRealm, std::uint16_t runesFound,
               bool goldLeft);
    /** Tells it whether gold still lies about: the wizard waits the long wait for it to be
     * gathered, and no longer than the short one once it is. */
    void setGoldLeft(bool left);
    void clear();

    /** Moves it `ticks` on, told how long each page of the caption up is (none without
     * one). Returns the voices to start. */
    std::vector<VictoryVoice> update(int ticks, std::span<const std::size_t> pageLengths);

    Stage stage() const { return m_stage; }
    bool running() const { return m_stage != Stage::None && m_stage != Stage::Done; }
    bool finished() const { return m_stage == Stage::Done; }
    /** The wizard's opacity, none until he comes. */
    float wizardAlpha() const { return m_alpha; }
    bool wizardShown() const { return m_stage >= Stage::Appearing && m_stage <= Stage::Gone; }
    /** The caption up, if any. */
    const std::optional<VictoryCaption>& caption() const { return m_caption; }
    /** Whether the teleport's sparkle is on the party. */
    bool sparkling() const { return m_sparkling; }
    /** How the realm's runestones went: 0 none found, 1 some, 2 the one, 3 both. */
    int runeQuality() const { return m_quality; }

    /** The defeat message of a boss kind (e.g. `LICH_SPEECH`), empty for one with none. */
    static std::string_view defeatMessageOf(int kind);
    /** The rune message for a quality. */
    static std::string_view runeMessageOf(int quality);
    /** The voice of a boss's defeat and of its rune line. */
    static std::string defeatVoiceOf(int kind, char realm);
    static std::string runeVoiceOf(int kind, char realm, int quality);
    /** The quality of `found` against a realm's `all`. */
    static int qualityOf(std::uint16_t all, std::uint16_t found);

private:
    void say(std::string_view message, int pauseAfter, const std::string& voice,
             std::vector<VictoryVoice>& voices);
    /** Types the caption on; true once every page has been read and the pause is over. */
    bool type(int ticks, std::span<const std::size_t> pageLengths);
    void leave();

    Stage m_stage = Stage::None;
    int m_kind = -1;
    char m_realm = 'G';
    int m_quality = 0;
    bool m_goldLeft = false;
    int m_ticksLeft = 0;
    float m_alpha = 0.0f;
    std::optional<VictoryCaption> m_caption;
    int m_typeTicks = 0;
    int m_pauseTicks = 0;
    int m_pauseAfter = 0;
    bool m_pageDone = false;
    bool m_pagesOver = false;
    bool m_sparkling = false;
};

} // namespace gdl::game
