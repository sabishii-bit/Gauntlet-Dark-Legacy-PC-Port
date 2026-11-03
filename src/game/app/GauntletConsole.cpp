#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/core/Log.h"
#include "engine/math/Math.h"
#include "engine/world/WorldCamera.h"

#include "game/app/ConsoleActionRoute.h"
#include "game/app/ConsoleRoster.h"
#include "game/app/Gauntlet.h"
#include "game/characters/CharacterSave.h"
#include "game/console/ConsolePolicy.h"
#include "game/console/NoclipMotion.h"
#include "game/players/PlayerActor.h"
#include "game/scenes/SceneDescription.h"
#include "game/scenes/SceneSelection.h"
#include "game/widgets/MenuInput.h"

namespace gdl::game {

void Gauntlet::setUpConsole() {
    if (!m_console.load(renderDevice(), m_options.unpackedDirectory)) {
        log::warn("Developer console: the 8x8 font is missing; its screen will be blank");
    }
    m_console.print(std::format("CGaunt++ {}", m_buildLabel.text()));
    m_console.print("Type help for the commands.");
    const auto playScene = [this]() -> PlayScene* {
        return m_play->scene.isOpen() ? &m_play->scene : nullptr;
    };
    const ConsoleRosterAccess access{
        .online = {.permitGuests =
                       [this](bool allow) {
                           return m_online && m_online->consoleHost() &&
                                  m_online->setConsolePermissions(allow);
                       }},
        .view = {.levelDebug = m_levelDebug,
                 .hudHidden = m_hudHidden,
                 .freeCamera = m_freeCamera,
                 .view = [this]() -> std::optional<WorldCamera> {
                     const PlayScene* scene = consoleScene();
                     return scene != nullptr ? std::optional{scene->viewCamera()} : std::nullopt;
                 },
                 .levelDebugLines = [this] { return levelDebugLines(); }},
        .listings = {.viewed = [this] { return consoleWorld(); },
                     .levels = m_levels,
                     .unpackedRoot = m_options.unpackedDirectory,
                     .save = [this]() -> const CharacterSave* {
                         const PlayScene* scene = consoleScene();
                         const PlayerActor* actor =
                             scene != nullptr ? scene->actor(consolePlayer()) : nullptr;
                         return actor != nullptr ? &actor->save() : nullptr;
                     }},
        .fixtures = {.scene = playScene, .world = [this] { return consoleWorld(); }},
        .shared = {.sink = m_consoleRoute, .levels = m_levels},
        .inspect = {
            .selected = [this]() -> std::optional<SceneSelection> {
                const PlayScene* scene = consoleScene();
                return scene != nullptr ? scene->selection() : std::nullopt;
            },
            .describe = [this](const SceneSelection& selection) -> std::vector<std::string> {
                const PlayScene* scene = consoleScene();
                const LevelWorld* world = consoleWorld();
                if (scene == nullptr || world == nullptr) {
                    return {"Nothing is in play."};
                }
                // Online the scene is the shared game's, whose models are never decoded here.
                return describeSelection(*scene, *world, selection,
                                         m_online ? nullptr : &m_play->world);
            }},
        .partyCount = {.count = m_partyCountOverride, .scene = playScene},
        .profile = {.saves = m_saves, .config = m_config, .scene = playScene},
    };
    registerConsoleRoster(m_console.host(), m_console.commands(), m_consoleListings, access,
                          [this](ConsoleHost& host, ConsoleListings& listings) {
                              registerLegendsConsole(host, listings);
                          });
}

ConsoleSession Gauntlet::consoleSession() const {
    ConsoleSituation situation{.netplay = m_netplay != nullptr};
    if (m_online) {
        situation.online = true;
        situation.host = m_online->consoleHost();
        situation.allowed = m_online->consoleAllowed();
    }
    if (const auto hold = legendsConsoleHold()) {
        situation.preview = *hold;
        situation.parked = true; // the preview holds the scene it was opened over
    }
    return ConsolePolicy::session(situation);
}

const PlayScene* Gauntlet::consoleScene() const {
    return m_online ? m_online->consoleScene()
                    : (m_play->scene.isOpen() ? &m_play->scene : nullptr);
}

const LevelWorld* Gauntlet::consoleWorld() const {
    return m_online ? m_online->consoleWorld()
                    : (m_play->scene.isOpen() ? &m_play->world : nullptr);
}

ConsoleActionRoute Gauntlet::consoleActionRoute() {
    return {[this](s32 seat, std::string line) -> std::optional<bool> {
                if (!m_online) {
                    return std::nullopt;
                }
                return seat >= 0 &&
                       m_online->consoleCommand(static_cast<u8>(seat), std::move(line));
            },
            [this] { return consolePlayer(); }};
}

bool Gauntlet::updateOnlineConsole(f64 deltaSeconds, bool overlay) {
    const bool wasOpen = m_console.open();
    const bool allowed = m_online && m_online->consoleAllowed() && !overlay;
    if (!allowed) {
        m_console.close();
        m_freeCamera.stop();
        window().holdCursor(false);
    } else {
        if (const auto* scene = consoleScene()) {
            if (const auto* actor = scene->actor(consolePlayer())) {
                m_console.setPrompt(actor->save().name + ">");
            }
        }
        m_console.setSession(consoleSession());
        m_console.update(deltaSeconds, input(), m_config.menu.console);
        if (m_console.open() && !wasOpen) {
            window().stopRumble();
            resetPlayInput();
        }
        // Over the screen the pointer, once it moves, sees through the console and a click
        // there picks what is beneath; elsewhere a click picks as before.
        const Vec2 cursor{input().pointer().x, input().pointer().y};
        const bool over = input().pointer().inside && consoleCovers(cursor);
        m_console.hover(cursor, over);
        if (m_console.open() && input().wasKeyPressed(Key::MouseLeft) &&
            (!over || m_console.seeThrough())) {
            consoleSelect(cursor);
        }
        if (m_freeCamera.active() && !m_console.open()) {
            m_freeCamera.update(input(), static_cast<f32>(deltaSeconds));
        }
        window().holdCursor(m_freeCamera.active() && !m_console.open() &&
                            input().isKeyDown(Key::MouseRight));
    }
    if (!allowed || (wasOpen && !m_console.open())) {
        consoleSelect(std::nullopt);
    }
    if (m_online) {
        m_online->consoleCapture(m_console.open() || wasOpen || m_freeCamera.active());
        m_online->consoleView(m_hudHidden, m_freeCamera.active()
                                               ? std::optional{m_freeCamera.camera()}
                                               : std::nullopt);
        for (const auto& reply : m_online->takeConsoleReplies()) {
            m_console.print(reply);
        }
    }
    return m_console.open() || wasOpen;
}

bool Gauntlet::consoleCovers(Vec2 cursor) {
    // The 2D layer is letterboxed into the window: as wide or as tall as the window
    // allows at the frame's shape, centred. The console spans that frame's width.
    const Extent2D window = renderDevice().framebufferExtent();
    if (window.isZero()) {
        return false;
    }
    const auto frameWidth = static_cast<f32>(m_config.display.frameWidth);
    const auto frameHeight = static_cast<f32>(m_config.display.frameHeight);
    const auto windowWidth = static_cast<f32>(window.width);
    const auto windowHeight = static_cast<f32>(window.height);
    const f32 scale = std::min(windowWidth / frameWidth, windowHeight / frameHeight);
    const f32 width = frameWidth * scale / windowWidth;
    const f32 height = frameHeight * scale / windowHeight;
    const f32 left = (1.0f - width) * 0.5f;
    const f32 top = (1.0f - height) * 0.5f;
    const f32 shown = height * m_console.slide() * static_cast<f32>(DevConsole::kHeight) /
                      static_cast<f32>(m_config.display.virtualHeight);
    return cursor.x >= left && cursor.x <= left + width && cursor.y >= top &&
           cursor.y <= top + shown;
}

void Gauntlet::consoleSelect(std::optional<Vec2> cursor) {
    std::optional<SceneSelection> picked;
    if (cursor.has_value() && consoleScene()) {
        picked = m_online ? m_online->consolePick(*cursor) : m_play->scene.pick(*cursor);
    }
    if (m_online) {
        m_online->consoleSelect(picked);
    } else if (m_play->scene.isOpen()) {
        m_play->scene.select(picked);
    }
    m_console.setVariable("SELECTED",
                          picked.has_value() ? std::optional{picked->token()} : std::nullopt);
    if (cursor.has_value()) {
        m_console.print(picked.has_value() ? std::format("Selected {}", picked->label())
                                           : "Nothing there to select");
    }
}

void Gauntlet::applyConsoleState(f64 deltaSeconds, bool consoleOpen) {
    m_play->scene.setHudHidden(m_hudHidden);
    if (m_freeCamera.active() && !consoleOpen) {
        m_freeCamera.update(input(), static_cast<f32>(deltaSeconds));
    }
    // While the free camera is turned by the right button, the pointer is hidden where
    // it is and shows there again when the button is let go.
    window().holdCursor(m_freeCamera.active() && !consoleOpen &&
                        input().isKeyDown(Key::MouseRight));
    m_play->scene.setViewOverride(m_freeCamera.active() ? std::optional{m_freeCamera.camera()}
                                                        : std::nullopt);
}

Vec3 Gauntlet::consoleFlight(s32 slot, f64 seconds, bool consoleOpen) {
    NoclipMotion& flight = m_noclip[static_cast<usize>(slot)];
    // The slot flies the seat it plays in the scene being stepped: online the seat its device
    // holds, offline the player of that number.
    const PlayScene* scene = consoleScene();
    bool flying = false;
    if (scene != nullptr) {
        const std::optional<u8> seat =
            m_online ? m_online->consoleSeat(slot) : std::optional{static_cast<u8>(slot)};
        flying = seat.has_value() && scene->cheats(*seat).noclip;
    }
    // No flight while its seat's noclip is off or the console is down.
    if (!flying || consoleOpen) {
        flight.reset();
        return Vec3{0.0f};
    }
    // Noclip flies a character as the camera looks, the console's keys left alone.
    const MenuInputSource source = playerInputSource(input(), m_config, slot);
    const NoclipMotion::Wish wish = NoclipMotion::wish(
        input(), playBindings(m_config, slot), source.keyboard, source.pad, scene->viewCamera());
    return flight.fly(wish, static_cast<f32>(seconds));
}

void Gauntlet::resetConsoleFlights() {
    for (NoclipMotion& flight : m_noclip) {
        flight.reset();
    }
}

/** The player the console speaks for: the one at the keyboard, else the first in. */
s32 Gauntlet::consolePlayer() {
    s32 first = -1;
    if (m_online) {
        for (s32 device = 0; device < PlayScene::kPlayerCount; ++device) {
            const auto seat = m_online->consoleSeat(device);
            if (!seat || !consoleScene() || !consoleScene()->actor(*seat)) {
                continue;
            }
            if (first < 0) {
                first = *seat;
            }
            if (playerInputSource(input(), m_config, device).keyboard) {
                return *seat;
            }
        }
        return first;
    }
    for (s32 player = 0; player < PlayScene::kPlayerCount; ++player) {
        if (m_play->scene.actor(player) == nullptr) {
            continue;
        }
        if (first < 0) {
            first = player;
        }
        if (playerInputSource(input(), m_config, player).keyboard) {
            return player;
        }
    }
    return first;
}

} // namespace gdl::game
