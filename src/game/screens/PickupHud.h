#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/ui/Canvas.h"

#include "game/screens/StatusBox.h"

namespace gdl::game {

/** A card sliding up over a player's status box to say what they picked up. */
struct PickupCard {
    enum class State : std::uint8_t { Rising, Holding, Falling, Done };

    std::int32_t player = 0;
    std::string texture; ///< the STATIC card, such as CRYSTAL
    std::int32_t y = 0;  ///< the strip's top; the card hangs under it
    State state = State::Rising;
    std::int32_t holdTicks = 0;
};

/** The count shown above a player's box after a collection: what they have of what they
 * need, beside an icon, for a while. */
struct PickupCount {
    std::string icon; ///< the STATIC icon
    std::int32_t count = 0;
    std::int32_t total = 0;
    float secondsLeft = 0.0f;

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
    static constexpr std::size_t kSlots = 4;
    static constexpr std::size_t kMostCards = 24;
    static constexpr std::int32_t kCardStartY = 384; ///< the strip starts just under the screen
    static constexpr std::int32_t kCardRestY = 304;  ///< and stops on the bar over the box
    static constexpr std::int32_t kCardEndY = 400;   ///< fallen past this it is gone
    static constexpr std::int32_t kCardHoldTicks = 90;
    static constexpr float kCountSeconds = 3.0f;
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
    static std::string_view crystalIcon(std::int32_t realm);

    /** Starts a card for `player`; ignored when every card is in use. */
    void addCard(std::int32_t player, std::string_view texture);
    /** Shows `player` `count` of `total` beside `icon` for the ticker's time. */
    void showCount(std::int32_t player, std::string_view icon, std::int32_t count,
                   std::int32_t total);
    /** Moves the cards `ticks` on and the tickers `seconds`. */
    void step(std::int32_t ticks, float seconds);
    void clear();
    /** Drops whatever `player` has up. */
    void clearPlayer(std::int32_t player);

    const std::vector<PickupCard>& cards() const { return m_cards; }
    const PickupCount& count(std::int32_t player) const;

    /** Draws the cards over the boxes and the tickers above them. */
    void draw(Canvas& canvas, StatusBoxPainter& boxes) const;

private:
    std::vector<PickupCard> m_cards;
    std::array<PickupCount, kSlots> m_counts{};
};

} // namespace gdl::game
