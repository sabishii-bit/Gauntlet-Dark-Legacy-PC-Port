#include <algorithm>
#include <chrono>

#include "game/app/Gauntlet.h"
#include "game/netplay/AssetDigest.h"

namespace gdl::game {
void Gauntlet::connectNetplay() {
    auto& ui = *m_netplay;
    ui.notice.clear();
    ui.view.status = "Checking native assets...";
    if (ui.digest.empty()) {
        std::promise<std::string> promise;
        ui.digestResult = promise.get_future();
        ui.digestWorker =
            std::jthread([root = m_options.unpackedDirectory,
                          result = std::move(promise)](const std::stop_token& stop) mutable {
                try {
                    result.set_value(assetDigest(root, stop));
                } catch (...) {
                    result.set_exception(std::current_exception());
                }
            });
        return;
    }
    m_online = std::make_unique<OnlineRun>();
    if (!m_online->openLobby(renderDevice(), context(), ui.pendingInvitation, m_version, ui.digest,
                             ui.view.localPlayers)) {
        ui.notice = m_online->status();
        m_online.reset();
    }
}
void Gauntlet::leaveNetplay() {
    m_select.close();
    m_online.reset();
    m_netplay.reset();
    resetPlayInput();
    setMaxFrameRate(m_config.display.maxFrameRate);
    if (!m_title.isOpen()) {
        startTitleScreen();
    }
}
void Gauntlet::updateNetplay(f64 deltaSeconds) {
    using Page = NetplayMenu::Page;
    using Action = NetplayMenu::Action;
    auto& ui = *m_netplay;
    if (ui.digestResult.valid() &&
        ui.digestResult.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            ui.digest = ui.digestResult.get();
            connectNetplay();
        } catch (const std::exception& error) {
            ui.notice = std::string("Asset check failed: ") + error.what();
        }
    }
    auto menu = ui.overlay && (ui.view.page == Page::Pause || ui.view.page == Page::Leave)
                    ? readPauseMenuInput(input(), m_config, ui.pauseOwner)
                    : readSharedMenuInput(input(), m_config);
    if (m_online) {
        if (!ui.overlay && (menu.start || menu.escape)) {
            ui.pauseOwner = playerPressingStart();
            if (m_online->pause(ui.pauseOwner)) {
                ui.overlay = true;
                ui.view.page = Page::Pause;
                ui.notice.clear();
                menu = {}; // Opening the menu must not immediately confirm Resume.
                window().stopRumble();
            }
        }
        m_online->update(m_select.isOpen() ? SessionInputs::Frame{} : readPlayInputs(deltaSeconds),
                         {});
        if (m_select.isOpen()) {
            updateSelect(deltaSeconds);
            return;
        }
        if (!m_online->playing() && ui.view.page != Page::Leave) {
            ui.view = m_online->lobby();
            ui.overlay = true;
        } else if (ui.view.page == Page::Host || ui.view.page == Page::Guest) {
            ui.overlay = false;
            setMaxFrameRate(m_config.timing.gameplayFrameRate);
        }
    }
    if (!ui.overlay) {
        return;
    }
    if (!ui.notice.empty()) {
        ui.view.status = ui.notice;
    }
    if (ui.view.page == Page::Pause) {
        ui.view.status = "Only your controls are paused. Your character can still be hurt.";
        ui.view.invitation.clear();
    }
    ui.menu.show(renderDevice(), ui.view);
    const auto event = ui.menu.update(menu);
    const auto cycle = [&](u8 value, s32 count, s32 minimum = 0) {
        return static_cast<u8>(
            (static_cast<s32>(value) - minimum + event.direction + count) % count + minimum);
    };
    switch (event.action) {
    case Action::Host:
        ui.view.page = Page::Host;
        ui.pendingInvitation.clear();
        connectNetplay();
        break;
    case Action::Join: ui.view.page = Page::Join; break;
    case Action::LocalPlayers: ui.view.localPlayers = cycle(ui.view.localPlayers, 4, 1); break;
    case Action::Paste: {
        auto code = window().clipboardText();
        const auto first = code.find_first_not_of(" \r\n\t");
        const auto last = code.find_last_not_of(" \r\n\t");
        code = first == std::string::npos ? "" : code.substr(first, last - first + 1);
        if (code.empty() || code.size() > 2048 || std::ranges::any_of(code, [](char c) {
                return c <= 32 || static_cast<u8>(c) >= 127;
            })) {
            ui.notice = "Clipboard does not contain a valid invitation code.";
        } else {
            ui.view.invitation = std::move(code);
            ui.notice.clear();
        }
        break;
    }
    case Action::Clear: ui.view.invitation.clear(); break;
    case Action::Connect:
        ui.pendingInvitation = ui.view.invitation;
        ui.view.page = Page::Guest;
        connectNetplay();
        break;
    case Action::Copy:
        window().setClipboardText(ui.view.invitation);
        ui.notice = "Invitation copied. Share it privately with your friends.";
        break;
    case Action::SelectCharacters:
        ui.notice.clear();
        if (startPlayerSelect(playerPressingStart())) {
            m_title.releaseMusic();
            m_title.close();
            resetPlayInput();
        }
        break;
    case Action::Ready:
        if (m_online) {
            m_online->ready(!ui.view.ready);
        }
        break;
    case Action::Start:
        if (m_online) {
            m_online->start();
        }
        break;
    case Action::Capacity:
    case Action::Difficulty:
    case Action::FriendlyFire: {
        auto rules = ui.view.settings;
        if (event.action == Action::Capacity) {
            rules.maxPlayers = cycle(rules.maxPlayers, 4, 1);
        }
        if (event.action == Action::Difficulty) {
            rules.difficulty = cycle(rules.difficulty, 3);
        }
        if (event.action == Action::FriendlyFire) {
            rules.friendlyFire = cycle(rules.friendlyFire, 3);
        }
        if (m_online && !m_online->settings(rules)) {
            ui.notice = "The player limit cannot be lower than the joined party.";
        } else {
            ui.notice = "Room settings changed. Everyone must ready again.";
        }
        break;
    }
    case Action::Resume:
        if (m_online) {
            m_online->resume();
        }
        ui.overlay = false;
        resetPlayInput();
        break;
    case Action::Leave:
        if (ui.view.page == Page::Leave) {
            leaveNetplay();
            return;
        }
        ui.previous = ui.view.page;
        ui.view.page = Page::Leave;
        ui.notice = ui.previous == Page::Host ? "Leaving closes the room for everyone."
                                              : "Disconnect from this game?";
        break;
    case Action::Back:
        if (ui.view.page == Page::Pause) {
            if (m_online) {
                m_online->resume();
            }
            ui.overlay = false;
            resetPlayInput();
        } else if (ui.view.page == Page::Leave) {
            ui.view.page = ui.previous;
            ui.notice.clear();
        } else if (ui.view.page == Page::Join) {
            ui.view.page = Page::Choose;
            ui.view.invitation.clear();
            ui.notice.clear();
        } else if (ui.view.page == Page::Choose || !m_online) {
            leaveNetplay();
            return;
        } else {
            ui.previous = ui.view.page;
            ui.view.page = Page::Leave;
            ui.notice = "Disconnect from this game?";
        }
        break;
    default: break;
    }
    if (!ui.notice.empty()) {
        ui.view.status = ui.notice;
    }
    ui.menu.show(renderDevice(), ui.view);
}
} // namespace gdl::game
