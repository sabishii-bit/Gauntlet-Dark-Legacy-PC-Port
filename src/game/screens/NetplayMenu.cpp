#include "game/screens/NetplayMenu.h"

#include <algorithm>
#include <array>
#include <format>

#include "engine/ui/Canvas.h"
#include "engine/ui/SystemFont.h"

namespace gdl::game {
namespace {
std::unique_ptr<Texture> text(RenderDevice& device, const std::string& label) {
    if (label.empty()) {
        return {};
    }
    auto printable = label.substr(0, 220);
    for (auto& c : printable) {
        if (c < 32 || static_cast<u8>(c) > 126) {
            c = '?';
        }
    }
    const auto image = rasterizeSystemText(printable, 32);
    return device.createTexture(
        {image.width, image.height, TextureFilter::Linear, TextureWrap::ClampToEdge}, image.pixels);
}
void draw(Canvas& canvas, const Texture* texture, f32 x, f32 y, f32 width, f32 height,
          Color color = Color::white()) {
    if (texture == nullptr) {
        return;
    }
    const f32 scale = std::min(width / static_cast<f32>(texture->width()),
                               height / static_cast<f32>(texture->height()));
    canvas.draw(*texture,
                {x, y, static_cast<f32>(texture->width()) * scale,
                 static_cast<f32>(texture->height()) * scale},
                color);
}
} // namespace
void NetplayMenu::show(RenderDevice& device, const View& view) {
    if (m_open && m_view == view) {
        return;
    }
    const bool changedPage = !m_open || m_view.page != view.page;
    m_open = true;
    m_view = view;
    m_rows.clear();
    const auto row = [&](std::string label, Action action, bool enabled = true,
                         bool adjustable = false) {
        m_rows.push_back({std::move(label), action, enabled, adjustable, {}});
    };
    std::string title = "Netplay";
    switch (view.page) {
    case Page::Choose:
        row("Host Game", Action::Host, view.available);
        row("Join Game", Action::Join, view.available);
        row(std::format("Local players    < {} >", view.localPlayers), Action::LocalPlayers,
            view.available, true);
        row("Back", Action::Back);
        break;
    case Page::Host:
    case Page::Guest: {
        const bool host = view.page == Page::Host;
        title = host ? "Host Game" : "Joined Game";
        static constexpr std::array<const char*, 3> kDifficulty{"Easy", "Normal", "Hard"};
        static constexpr std::array<const char*, 3> kDamage{"Off", "Stun", "Damage"};
        row(host ? "Copy invitation code" : "Connected to host", Action::Copy,
            host && !view.invitation.empty());
        row(std::format("Player limit    < {} >    ({} joined)", view.settings.maxPlayers,
                        view.players),
            Action::Capacity, host && view.connected, true);
        row(std::format("Difficulty    < {} >", kDifficulty[view.settings.difficulty]),
            Action::Difficulty, host && view.connected, true);
        row(std::format("Friendly fire    < {} >", kDamage[view.settings.friendlyFire]),
            Action::FriendlyFire, host && view.connected, true);
        row(std::format("Select {} local character(s)", view.localPlayers),
            Action::SelectCharacters, view.connected && !view.ready);
        row(view.ready ? "Not Ready" : "Ready", Action::Ready, view.connected && view.selected);
        if (host) {
            row("Start Game", Action::Start, view.canStart);
        }
        row("Leave Room", Action::Leave);
        break;
    }
    case Page::Join:
        title = "Join Game";
        row("Paste invitation code", Action::Paste);
        row("Clear code", Action::Clear, !view.invitation.empty());
        row("Connect", Action::Connect, !view.invitation.empty());
        row("Back", Action::Back);
        break;
    case Page::Pause:
        title = "Online Menu - game continues";
        row("Resume", Action::Resume);
        row("Leave Game", Action::Leave);
        break;
    case Page::Leave:
        title = "Leave the online game?";
        row("Cancel", Action::Back);
        row("Leave Game", Action::Leave);
        break;
    }
    m_title = text(device, title);
    m_notice = text(device, view.status);
    // The complete opaque ticket is copied, never truncated for transport or logged. Its
    // preview deliberately elides the middle so it cannot force tiny unreadable menu text.
    std::string code = view.invitation;
    if (code.size() > 58) {
        code = code.substr(0, 38) + "..." + code.substr(code.size() - 16);
    }
    m_code = text(device, code.empty() ? "" : "Code: " + code);
    for (auto& item : m_rows) {
        item.texture = text(device, item.label);
    }
    if (changedPage || m_selection >= m_rows.size()) {
        m_selection = 0;
    }
    for (usize i = 0; i < m_rows.size() && !m_rows[m_selection].enabled; ++i) {
        m_selection = (m_selection + 1) % m_rows.size();
    }
}
Rect NetplayMenu::rowArea(usize row) {
    return {44, 100 + static_cast<f32>(row) * 27, 424, 24};
}
NetplayMenu::Event NetplayMenu::update(const MenuInput& raw) {
    if (!m_open) {
        return {};
    }
    const auto input = mapMenuPointer(raw, m_pointerTransform);
    if (input.back || input.escape || input.pointerBack) {
        return {Action::Back};
    }
    if (input.pointer && (input.pointer != m_pointer || input.pointerPressed)) {
        for (usize i = 0; i < m_rows.size(); ++i) {
            const auto area = rowArea(i);
            if (m_rows[i].enabled && input.pointer->x >= area.x &&
                input.pointer->x < area.right() && input.pointer->y >= area.y &&
                input.pointer->y < area.bottom()) {
                m_selection = i;
                if (input.pointerPressed) {
                    return {m_rows[i].action, 1};
                }
            }
        }
        m_pointer = input.pointer;
    }
    if (input.up || input.down || input.pointerScroll != 0) {
        const bool up = input.up || input.pointerScroll > 0;
        for (usize i = 0; i < m_rows.size(); ++i) {
            m_selection = (m_selection + (up ? m_rows.size() - 1 : 1)) % m_rows.size();
            if (m_rows[m_selection].enabled) {
                break;
            }
        }
    }
    const auto& row = m_rows[m_selection];
    if (row.enabled &&
        (input.select || input.start || (row.adjustable && (input.left || input.right)))) {
        return {row.action, input.left ? -1 : 1};
    }
    return {};
}
void NetplayMenu::render(RenderDevice& device, const Mat4& projection, f32 width, f32 height) {
    m_pointerTransform = makeVirtualScreenTransform(projection, 512, 384, width, height);
    Canvas canvas;
    canvas.begin(device, m_pointerTransform, {.depthWrite = false, .depthTest = false});
    canvas.fillScreen(Color::rgba(0, 0, 0, 190));
    canvas.fill({28, 30, 456, 324}, Color::rgba(15, 15, 27, 245));
    draw(canvas, m_title.get(), 44, 42, 424, 23);
    draw(canvas, m_code.get(), 44, 76, 424, 14);
    for (usize i = 0; i < m_rows.size(); ++i) {
        const auto area = rowArea(i);
        if (i == m_selection) {
            canvas.fill(area, Color::rgba(92, 25, 130, 220));
        }
        draw(canvas, m_rows[i].texture.get(), area.x + 6, area.y + 3, area.width - 12, 18,
             m_rows[i].enabled ? Color::white() : Color::rgba(130, 130, 130));
    }
    draw(canvas, m_notice.get(), 44, 331, 424, 15);
    canvas.end();
}
} // namespace gdl::game
