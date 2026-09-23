#include "game/screens/PickupHud.h"

#include <algorithm>

#include "engine/core/Types.h"

namespace gdl::game {

std::string_view PickupHud::crystalIcon(s32 realm) {
    if (realm <= 0 || static_cast<usize>(realm) >= kCrystalIcons.size()) {
        return {};
    }
    return kCrystalIcons[static_cast<usize>(realm)];
}

void PickupHud::addCard(s32 player, std::string_view texture) {
    if (m_cards.size() >= kMostCards || player < 0 || static_cast<usize>(player) >= kSlots) {
        return;
    }
    PickupCard card;
    card.player = player;
    card.texture = std::string(texture);
    card.y = kCardStartY;
    m_cards.push_back(std::move(card));
}

void PickupHud::showCount(s32 player, std::string_view icon, s32 count, s32 total) {
    if (player < 0 || static_cast<usize>(player) >= kSlots) {
        return;
    }
    PickupCount& shown = m_counts[static_cast<usize>(player)];
    shown.icon = std::string(icon);
    shown.count = count;
    shown.total = total;
    shown.secondsLeft = kCountSeconds;
}

void PickupHud::step(s32 ticks, f32 seconds) {
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

void PickupHud::clearPlayer(s32 player) {
    std::erase_if(m_cards, [player](const PickupCard& card) { return card.player == player; });
    if (player >= 0 && static_cast<usize>(player) < kSlots) {
        m_counts[static_cast<usize>(player)] = PickupCount{};
    }
}

const PickupCount& PickupHud::count(s32 player) const {
    static const PickupCount kNone;
    if (player < 0 || static_cast<usize>(player) >= kSlots) {
        return kNone;
    }
    return m_counts[static_cast<usize>(player)];
}

void PickupHud::draw(Canvas& canvas, StatusBoxPainter& boxes) const {
    for (const PickupCard& card : m_cards) {
        boxes.drawCard(canvas, card.player, card.texture, card.y);
    }
    for (usize slot = 0; slot < kSlots; ++slot) {
        const PickupCount& shown = m_counts[slot];
        if (shown.showing()) {
            boxes.drawCount(canvas, static_cast<s32>(slot), shown.icon, shown.count, shown.total);
        }
    }
}

} // namespace gdl::game
