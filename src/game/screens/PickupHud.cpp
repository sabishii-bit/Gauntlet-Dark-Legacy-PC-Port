#include "game/screens/PickupHud.h"

#include <algorithm>
#include <cstddef>

namespace gdl::game {

std::string_view PickupHud::crystalIcon(int realm) {
    if (realm <= 0 || static_cast<std::size_t>(realm) >= kCrystalIcons.size()) {
        return {};
    }
    return kCrystalIcons[static_cast<std::size_t>(realm)];
}

void PickupHud::addCard(int player, std::string_view texture) {
    if (m_cards.size() >= kMostCards || player < 0 || static_cast<std::size_t>(player) >= kSlots) {
        return;
    }
    PickupCard card;
    card.player = player;
    card.texture = std::string(texture);
    card.y = kCardStartY;
    m_cards.push_back(std::move(card));
}

void PickupHud::showCount(int player, std::string_view icon, int count, int total) {
    if (player < 0 || static_cast<std::size_t>(player) >= kSlots) {
        return;
    }
    PickupCount& shown = m_counts[static_cast<std::size_t>(player)];
    shown.icon = std::string(icon);
    shown.count = count;
    shown.total = total;
    shown.secondsLeft = kCountSeconds;
}

void PickupHud::step(int ticks, float seconds) {
    for (PickupCard& card : m_cards) {
        switch (card.state) {
        case PickupCard::State::Rising:
            card.y -= ticks;
            if (card.y <= kCardRestY) {
                card.y = kCardRestY;
                card.state = PickupCard::State::Holding;
                card.holdTicks = kCardHoldTicks;
            }
            break;
        case PickupCard::State::Holding:
            card.holdTicks -= ticks;
            if (card.holdTicks <= 0) {
                card.state = PickupCard::State::Falling;
            }
            break;
        case PickupCard::State::Falling:
            card.y += ticks;
            if (card.y >= kCardEndY) {
                card.state = PickupCard::State::Done;
            }
            break;
        case PickupCard::State::Done: break;
        }
    }
    std::erase_if(m_cards,
                  [](const PickupCard& card) { return card.state == PickupCard::State::Done; });
    for (PickupCount& shown : m_counts) {
        if (shown.showing()) {
            shown.secondsLeft = std::max(shown.secondsLeft - seconds, 0.0f);
        }
    }
}

void PickupHud::clear() {
    m_cards.clear();
    for (PickupCount& shown : m_counts) {
        shown = PickupCount{};
    }
}

void PickupHud::clearPlayer(int player) {
    std::erase_if(m_cards, [player](const PickupCard& card) { return card.player == player; });
    if (player >= 0 && static_cast<std::size_t>(player) < kSlots) {
        m_counts[static_cast<std::size_t>(player)] = PickupCount{};
    }
}

const PickupCount& PickupHud::count(int player) const {
    static const PickupCount kNone;
    if (player < 0 || static_cast<std::size_t>(player) >= kSlots) {
        return kNone;
    }
    return m_counts[static_cast<std::size_t>(player)];
}

void PickupHud::draw(Canvas& canvas, StatusBoxPainter& boxes) const {
    for (const PickupCard& card : m_cards) {
        boxes.drawCard(canvas, card.player, card.texture, card.y);
    }
    for (std::size_t slot = 0; slot < kSlots; ++slot) {
        const PickupCount& shown = m_counts[slot];
        if (shown.showing()) {
            boxes.drawCount(canvas, static_cast<int>(slot), shown.icon, shown.count, shown.total);
        }
    }
}

} // namespace gdl::game
