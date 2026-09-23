#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Types.h"
#include "engine/ui/Canvas.h"

#include "game/screens/StatusBox.h"

namespace gdl::game {

/** A card sliding up over a player's status box to say what they picked up. */
struct PickupCard {
    enum class State : u8 { Rising, Holding, Falling, Done };

    s32 player = 0;
    std::string texture; ///< the STATIC card, such as CRYSTAL
    s32 y = 0;           ///< the strip's top; the card hangs under it
    State state = State::Rising;
    s32 holdTicks = 0;
};

/** The count shown above a player's box after a collection: what they have of what they
 * need, beside an icon, for a while. */
struct PickupCount {
    std::string icon; ///< the STATIC icon
    s32 count = 0;
    s32 total = 0;
    f32 secondsLeft = 0.0f;

    bool showing() const { return secondsLeft > 0.0f; }
};

/**
 * What the original puts up on a pickup: a strip and a card slide up from under the screen
 * to cover the taker's status box, hold, and slide back down; and a crystal (or a coin, or
 * an icon) shows every player a small ticker above their box counting the realm's crystals
 * they have of those its gate wants, for three seconds.
 */
class PickupHud {
public:
    static constexpr usize kSlots = 4;
    static constexpr usize kMostCards = 24;
    static constexpr s32 kCardStartY = 384; ///< the strip starts just under the screen
    static constexpr s32 kCardRestY = 304;  ///< and stops on the bar over the box
    static constexpr s32 kCardEndY = 400;   ///< fallen past this it is gone
    static constexpr s32 kCardHoldTicks = 90;
    static constexpr f32 kCountSeconds = 3.0f;
    static constexpr std::string_view kCrystalCard = "CRYSTAL";
    /** The ticker's icon for each realm's crystal, by realm. */
    static constexpr std::array<std::string_view, 9> kCrystalIcons{"",
                                                                   "SM_CRYSTAL_ORA",
                                                                   "SM_CRYSTAL_RED",
                                                                   "SM_CRYSTAL_PUR",
                                                                   "SM_CRYSTAL_BLU",
                                                                   "SM_CRYSTAL_GRE",
                                                                   "SM_CRYSTAL_YEL",
                                                                   "SM_CRYSTAL_WHI",
                                                                   "SM_CRYSTAL_BLA"};

    /** The icon for a realm's crystal; empty for no realm. */
    static std::string_view crystalIcon(s32 realm);

    /** Starts a card for `player`; ignored when every card is in use. */
    void addCard(s32 player, std::string_view texture);
    /** Shows `player` `count` of `total` beside `icon` for the ticker's time. */
    void showCount(s32 player, std::string_view icon, s32 count, s32 total);
    /** Moves the cards `ticks` on and the tickers `seconds`. */
    void step(s32 ticks, f32 seconds);
    void clear();
    /** Drops whatever `player` has up. */
    void clearPlayer(s32 player);

    const std::vector<PickupCard>& cards() const { return m_cards; }
    const PickupCount& count(s32 player) const;

    /** Draws the cards over the boxes and the tickers above them. */
    void draw(Canvas& canvas, StatusBoxPainter& boxes) const;

private:
    std::vector<PickupCard> m_cards;
    std::array<PickupCount, kSlots> m_counts{};
};

} // namespace gdl::game
