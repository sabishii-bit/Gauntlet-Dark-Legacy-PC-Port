#include "game/hud/PickupHud.h"

#include <algorithm>

#include "engine/core/Types.h"

namespace gdl::game {

namespace {

template <class Counts> auto rowOf(Counts& all, s32 player) {
    return std::span(all).subspan(static_cast<usize>(player) * PickupHud::kCountsPerPlayer,
                                  PickupHud::kCountsPerPlayer);
}

/** The count that `icon` takes: its own while showing, else a free one, else the oldest. */
PickupCount& spotFor(std::span<PickupCount> row, std::string_view icon) {
    for (PickupCount& held : row) {
        if (held.showing() && held.icon == icon) {
            return held;
        }
    }
    for (PickupCount& held : row) {
        if (!held.showing()) {
            return held;
        }
    }
    return *std::ranges::min_element(row, {}, &PickupCount::secondsLeft);
}

} // namespace

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
    PickupCount& shown = spotFor(rowOf(m_counts, player), icon);
    shown.icon = std::string(icon);
    // A record under nought is one already congratulated: the HUD shows it full
    // (towerBossStatus returns the total for it, and the drawing clamps again).
    shown.count = count < 0 && total > 0 ? total : count;
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
        for (PickupCount& held : rowOf(m_counts, player)) {
            held = PickupCount{};
        }
    }
}

std::span<const PickupCount> PickupHud::counts(s32 player) const {
    if (player < 0 || static_cast<usize>(player) >= kSlots) {
        return {};
    }
    return rowOf(m_counts, player);
}

s32 PickupHud::countRowY(std::optional<s32> labelTop) {
    return labelTop ? *labelTop - kCountLabelGap - kCountHeight : kCountRestY;
}

void PickupHud::draw(Canvas& canvas, StatusBoxPainter& boxes,
                     const std::array<std::optional<s32>, kSlots>& labelTops) const {
    for (const PickupCard& card : m_cards) {
        boxes.drawCard(canvas, card.player, card.texture, card.y);
    }
    for (usize slot = 0; slot < kSlots; ++slot) {
        std::array<StatusBoxPainter::CountEntry, kCountsPerPlayer> row;
        usize shown = 0;
        for (const PickupCount& held : counts(static_cast<s32>(slot))) {
            if (held.showing()) {
                row.at(shown++) = {.icon = held.icon, .count = held.count, .total = held.total};
            }
        }
        boxes.drawCounts(canvas, static_cast<s32>(slot),
                         std::span<const StatusBoxPainter::CountEntry>(row).first(shown),
                         countRowY(labelTops.at(slot)));
    }
}

} // namespace gdl::game
