#include "game/online/NetplayMenu.h"

#include <algorithm>
#include <array>
#include <format>
#include <string_view>

#include "engine/core/Log.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/SystemFont.h"

#include "game/config/GameConfig.h"

namespace gdl::game {
namespace {
/** The friendly-fire rule's words, by `RoomSettings::friendlyFire`. */
constexpr std::array<std::string_view, 3> kFriendlyFireText{
    "netplay.friendlyFire.off", "netplay.friendlyFire.stun", "netplay.friendlyFire.damage"};
/** The camera rule's words, by `RoomSettings::camera`. */
constexpr std::array<std::string_view, 2> kCameraText{"netplay.camera.shared",
                                                      "netplay.camera.solo"};
/** A room rule's words, off then on, as the settings pages write a switch. */
constexpr std::array<std::string_view, 2> kSwitchText{"settings.off", "settings.on"};

/** A table entry with its `{}` filled in; one that does not format reads as written. */
template <typename... Values>
std::string filled(std::string_view pattern, const Values&... values) {
    try {
        return std::vformat(pattern, std::make_format_args(values...));
    } catch (const std::format_error& error) {
        log::warn("Netplay menu: text '{}' does not format: {}", pattern, error.what());
        return std::string(pattern);
    }
}
/** An adjustable row: its label, then its value between the arrows that change it. */
template <typename Value> std::string adjustableText(std::string_view label, const Value& value) {
    return std::format("{}    < {} >", label, value);
}

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
void NetplayMenu::show(RenderDevice& device, const StringTable& strings, const View& view) {
    if (m_open && m_view == view) {
        return;
    }
    const bool changedPage = !m_open || m_view.page != view.page;
    m_open = true;
    m_view = view;
    m_rows.clear();
    const auto row = [&](std::string label, Action action, bool enabled = true,
                         bool adjustable = false, u8 rule = 0) {
        m_rows.push_back({std::move(label), action, enabled, adjustable, {}, rule});
    };
    const auto words = [&](std::string_view id) { return std::string(strings.get(id)); };
    std::string title = words("netplay.title");
    switch (view.page) {
    case Page::Choose:
        row(words("netplay.host"), Action::Host, view.available);
        row(words("netplay.join"), Action::Join, view.available);
        row(adjustableText(words("netplay.localPlayers"), view.localPlayers), Action::LocalPlayers,
            view.available, true);
        row(words("netplay.back"), Action::Back);
        break;
    case Page::Host:
    case Page::Guest: {
        const bool host = view.page == Page::Host;
        title = words(host ? "netplay.host" : "netplay.joinedGame");
        row(words(host ? "netplay.copyInvitation" : "netplay.connected"), Action::Copy,
            host && !view.invitation.empty());
        row(std::format("{}    {}",
                        adjustableText(words("netplay.playerLimit"), view.settings.maxPlayers),
                        filled(words("netplay.joinedCount"), view.players)),
            Action::Capacity, host && view.connected, true);
        // The game's rules have a page of their own; a guest reads them there.
        row(words("netplay.gameSettings"), Action::GameSettings, view.connected);
        row(filled(words("netplay.selectCharacters"), view.localPlayers), Action::SelectCharacters,
            view.connected && !view.ready);
        row(words(view.ready ? "netplay.notReady" : "netplay.ready"), Action::Ready,
            view.connected && view.selected);
        if (host) {
            row(words("netplay.start"), Action::Start, view.canStart);
        }
        row(words("netplay.leaveRoom"), Action::Leave);
        break;
    }
    case Page::Settings: {
        // The host sets the game's rules for everyone; a guest sees them, greyed. The
        // difficulty reads as the Difficulty page writes it.
        title = words("netplay.gameSettings");
        const bool sets = view.host && view.connected;
        const std::string difficulty(DifficultyConfig::kNames[view.settings.difficulty]);
        row(adjustableText(words("settings.difficulty"), words("settings." + difficulty)),
            Action::Difficulty, sets, true);
        row(adjustableText(words("netplay.friendlyFire"),
                           words(kFriendlyFireText[view.settings.friendlyFire])),
            Action::FriendlyFire, sets, true);
        row(adjustableText(words("netplay.camera"), words(kCameraText[view.settings.camera & 1U])),
            Action::Camera, sets, true);
        for (usize bit = 0; bit < view.rules.size() && bit < 8; ++bit) {
            const u32 on = (static_cast<u32>(view.settings.mechanics) >> bit) & 1U;
            row(adjustableText(view.rules[bit], words(kSwitchText[on])), Action::Rule, sets, true,
                static_cast<u8>(bit));
        }
        row(words("netplay.back"), Action::Back);
        break;
    }
    case Page::Join:
        title = words("netplay.join");
        row(words("netplay.paste"), Action::Paste);
        row(words("netplay.clear"), Action::Clear, !view.invitation.empty());
        row(words("netplay.connect"), Action::Connect, !view.invitation.empty());
        row(words("netplay.back"), Action::Back);
        break;
    case Page::Pause:
        title = words("netplay.pauseTitle");
        row(words("netplay.resume"), Action::Resume);
        row(words("netplay.saveProgress"), Action::SaveCompletedProgress,
            view.canSaveCompletedProgress);
        row(words("netplay.leaveGame"), Action::Leave);
        break;
    case Page::Finished:
        title = words("netplay.finishedTitle");
        row(words("netplay.saveProgress"), Action::SaveCompletedProgress,
            view.canSaveCompletedProgress);
        row(words("netplay.returnTitle"), Action::ReturnTitle);
        break;
    case Page::Leave:
        title = words("netplay.leaveTitle");
        row(words("netplay.cancel"), Action::Back);
        row(words("netplay.leaveGame"), Action::Leave);
        break;
    }
    m_titleText = title;
    m_title = text(device, title);
    m_notice = text(device, view.status);
    // The complete opaque ticket is copied, never truncated for transport or logged. Its
    // preview deliberately elides the middle so it cannot force tiny unreadable menu text.
    std::string code = view.invitation;
    if (code.size() > 58) {
        code = code.substr(0, 38) + "..." + code.substr(code.size() - 16);
    }
    m_code = text(device, code.empty() ? "" : filled(words("netplay.code"), code));
    for (auto& item : m_rows) {
        item.texture = text(device, item.label);
    }
    m_pointerTargets.clear();
    for (usize i = 0; i < m_rows.size(); ++i) {
        if (m_rows[i].enabled) {
            m_pointerTargets.add(rowArea(i), i);
        }
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
        if (const auto* target = m_pointerTargets.at(*input.pointer)) {
            m_selection = target->control;
            if (input.pointerPressed) {
                const auto& row = m_rows[m_selection];
                return {row.action, 1, row.rule};
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
        return {row.action, input.left ? -1 : 1, row.rule};
    }
    return {};
}
void NetplayMenu::render(RenderDevice& device, const Mat4& projection, f32 width, f32 height) {
    m_pointerTransform = makeVirtualScreenTransform(projection, 512, 384, width, height);
    Canvas canvas;
    canvas.begin(device, m_pointerTransform, {.depthWrite = false, .depthTest = false, .fog = {}});
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
