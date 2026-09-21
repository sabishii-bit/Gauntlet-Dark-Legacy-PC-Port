#include "game/screens/HelpMessages.h"

#include <algorithm>

#include "game/menu/ScrollBox.h"

namespace gdl::game {

namespace {

constexpr std::array<HelpMessageSpec, 12> kSpecs{{
    {HelpMessages::kDoorNeedsKey, "USEKEYOPENDOOR", "S_USEKEY", false},
    {HelpMessages::kChestNeedsKey, "USEKEYOPENCHEST", "S_USEKEY2", false},
    {HelpMessages::kKeysFull, "FULLOFKEYS", "S_KEYFULL", false},
    {HelpMessages::kNoPotion, "COLLECTMAGICFIRST", "S_COLLECTPOT", false},
    {HelpMessages::kTrapsHurt, "AVOIDOBJECTS", "S_AVOID", false},
    {HelpMessages::kRandomChest, "RANDOMCHEST", "S_SILVER", false},
    {HelpMessages::kBarrelsHold, "WOODBARREL", "S_SOMEBARRELS", false},
    {HelpMessages::kUseTurbo, "USETURBO", "S_USETURBO", false},
    {HelpMessages::kHealthFull, "HEALTHFULL", "S_HEALTHFULL", true},
    {HelpMessages::kBlastsDestroy, "EXPDESTROY", "S_EXPDSTITMS", false},
    {HelpMessages::kGasSpoils, "GASPOISON", "S_GASFOODBAD", false},
    {HelpMessages::kChestsExplode, "CHESTSEXPL", "S_CHESTSEXPL", false},
}};

/** The original's ink for players one to four: dark yellow, blue, red and green. */
constexpr std::array<Color, 4> kInks{Color::rgba(0x1F, 0x1F, 0x00), Color::rgba(0x00, 0x00, 0x1F),
                                     Color::rgba(0x1F, 0x00, 0x00), Color::rgba(0x00, 0x1F, 0x00)};

} // namespace

const HelpMessageSpec* HelpMessages::specOf(s32 id) {
    const auto found = std::ranges::find(kSpecs, id, &HelpMessageSpec::id);
    return found != kSpecs.end() ? &*found : nullptr;
}

Color HelpMessages::inkOf(s32 player) {
    return player >= 0 && static_cast<usize>(player) < kInks.size()
               ? kInks[static_cast<usize>(player)]
               : ScrollBox::kTextColor;
}

void HelpMessages::clear() {
    m_lines.clear();
    m_id = -1;
    m_ticksLeft = 0;
    m_pauseLeft = 0;
    m_posted = 0;
}

const HelpMessageSpec* HelpMessages::post(s32 id, s32 player,
                                          std::span<const HelpReader> party) {
    const HelpMessageSpec* spec = specOf(id);
    if (spec == nullptr || showing() || m_pauseLeft > 0 || m_strings == nullptr) {
        return nullptr;
    }
    // Told once: a player's own message until that player has seen it, the others until
    // everyone playing has.
    const auto sawIt = [id](const HelpReader& reader) {
        return reader.seen != nullptr && std::ranges::binary_search(*reader.seen, id);
    };
    const auto concerns = [&](const HelpReader& reader) {
        return !spec->perPlayer || reader.player == player;
    };
    const bool wanted = std::ranges::any_of(
        party, [&](const HelpReader& reader) { return concerns(reader) && !sawIt(reader); });
    const auto message = m_strings->find(spec->text);
    if (!wanted || !message.has_value()) {
        return nullptr;
    }
    const MessageInfo& info = m_strings->message(*message);
    // The strings keep a message's lines as its pages.
    m_lines.clear();
    for (const std::string& page : info.pages) {
        for (std::string& line : ScrollBox::splitLines(page)) {
            m_lines.push_back(std::move(line));
        }
    }
    if (m_lines.empty()) {
        return nullptr;
    }
    for (const HelpReader& reader : party) {
        if (reader.seen != nullptr && concerns(reader) && !sawIt(reader)) {
            reader.seen->insert(std::ranges::upper_bound(*reader.seen, id), id);
        }
    }
    m_id = id;
    m_player = player;
    m_ticksLeft = static_cast<s32>(m_lines.size()) * kTicksPerLine + kTicksOver;
    return spec;
}

void HelpMessages::update(s32 ticks) {
    m_pauseLeft = std::max(m_pauseLeft - ticks, 0);
    if (m_ticksLeft <= 0) {
        return;
    }
    m_ticksLeft -= ticks;
    if (m_ticksLeft <= 0) {
        m_ticksLeft = 0;
        m_pauseLeft = kPauses[std::min(m_posted, kPauses.size() - 1)];
        ++m_posted;
    }
}

Rect HelpMessages::areaFor(const TextPainter& text, const Vec2& head) const {
    s32 widest = 0;
    for (const std::string& line : m_lines) {
        widest = std::max(widest, text.measure(line));
    }
    const auto width = static_cast<f32>(widest + kMarginAcross);
    const auto height =
        static_cast<f32>(static_cast<s32>(m_lines.size()) * text.lineHeight() + kMarginDown);
    const f32 left = std::clamp(head.x - width * 0.5f, 0.0f,
                                std::max(static_cast<f32>(kWidest) - width, 0.0f));
    const f32 top = std::clamp(head.y - static_cast<f32>(kAboveHead) - height * 0.5f, 2.0f,
                               std::max(static_cast<f32>(kLowest) - height, 2.0f));
    return Rect{left, top, width, height};
}

void HelpMessages::draw(Canvas& canvas, const TextPainter& text, const Texture* scroll,
                        const Vec2& head) const {
    if (!showing() || !text.ready()) {
        return;
    }
    const Rect area = areaFor(text, head);
    if (scroll != nullptr) {
        canvas.draw(*scroll, area, Color::white().withAlpha(kScrollAlpha));
    }
    TextStyle style;
    style.color = inkOf(m_player);
    const auto centre = static_cast<s32>(area.x + area.width * 0.5f);
    s32 y = static_cast<s32>(area.y) + kMarginDown / 2;
    for (const std::string& line : m_lines) {
        text.draw(canvas, -centre, y, line, style);
        y += text.lineHeight();
    }
}

} // namespace gdl::game
