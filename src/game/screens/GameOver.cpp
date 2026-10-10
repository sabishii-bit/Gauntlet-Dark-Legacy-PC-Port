#include "game/screens/GameOver.h"

#include <algorithm>

#include "engine/core/Types.h"

#include "game/screens/PlayerRuntime.h"

namespace gdl::game {

bool GameOver::ready(std::span<const PlayerRuntime> players) {
    return !players.empty() && std::ranges::all_of(players, [](const PlayerRuntime& player) {
        return player.life == PlayerLife::InTower;
    });
}

void GameOver::begin(std::string_view caption) {
    if (m_active) {
        return;
    }
    m_caption = caption;
    m_ticks = 0;
    m_active = true;
}

void GameOver::clear() {
    m_caption.clear();
    m_ticks = 0;
    m_active = false;
}

bool GameOver::step(s32 ticks) {
    if (!m_active || finished()) {
        return false;
    }
    const s32 before = m_ticks;
    m_ticks += std::clamp(ticks, 0, kDurationTicks - m_ticks);
    return before < kVoiceTick && m_ticks >= kVoiceTick;
}

usize GameOver::letters() const {
    return std::min(m_caption.size(),
                    static_cast<usize>(std::max(0, m_ticks - kVoiceTick) / kLetterTicks));
}

void GameOver::draw(Canvas& canvas, const TextPainter& text, f32 width) const {
    drawCaption(canvas, text, width, m_caption, m_active ? letters() : 0);
}

std::string_view GameOver::captionFor(const MessageTable& messages, const StringTable* strings) {
    if (strings != nullptr && strings->has(kTextId)) {
        return strings->get(kTextId);
    }
    if (const auto message = messages.find(kMessage)) {
        const auto& pages = messages.message(*message).pages;
        if (!pages.empty()) {
            return pages.front();
        }
    }
    return {};
}

void GameOver::drawCaption(Canvas& canvas, const TextPainter& text, f32 width,
                           std::string_view caption, usize letters) {
    if (letters == 0) {
        return;
    }
    // The full line is centred before its stored text is truncated for the reveal.
    const s32 x = text.leftEdge(-static_cast<s32>(width / 2), caption, kTextScale);
    text.draw(canvas, x, kTextY, caption.substr(0, letters), TextStyle{kTextScale, Color::white()});
}

} // namespace gdl::game
