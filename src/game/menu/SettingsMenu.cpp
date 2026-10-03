#include "game/menu/SettingsMenu.h"

#include <algorithm>
#include <array>
#include <utility>

#include "engine/core/Types.h"

namespace gdl::game {
namespace {
constexpr s32 kGraphicsCode = 4;
constexpr s32 kVideoRows = 6;
constexpr std::array<u32, 3> kFrameRates{30, 60, 0};
constexpr std::array<u32, 3> kSampleCounts{1, 2, 4};

u32 cycleChoice(const std::array<u32, 3>& choices, u32 value, s32 direction) {
    for (usize i = 0; i < choices.size(); ++i) {
        if (choices[i] == value) {
            const auto count = static_cast<s32>(choices.size());
            return choices[static_cast<usize>((static_cast<s32>(i) + direction + count) % count)];
        }
    }
    return choices.front();
}
} // namespace
std::string SettingsMenu::text(std::string_view id) const {
    return std::string(m_strings != nullptr ? m_strings->get(id) : id);
}
void SettingsMenu::open(const GameConfig& config, const StringTable* strings, Persist persist,
                        const TextPainter& painter, const MenuScreen& screen,
                        MenuDefinition backdrop, Scope scope, PreviewAudio preview,
                        DisplayOptions display, std::function<DisplayOptions()> queryDisplay,
                        Persist previewVideo, VideoSettings::Clock clock) {
    if (!m_video.revert()) {
        return;
    }
    m_config = config;
    m_strings = strings;
    m_persist = std::move(persist);
    m_previewAudio = std::move(preview);
    m_display = std::move(display);
    m_queryDisplay = std::move(queryDisplay);
    m_previewVideo = std::move(previewVideo);
    m_clock = std::move(clock);
    m_confirmVideo = false;
    m_audioDirty = false;
    m_scope = scope;
    m_painter = &painter;
    m_screen = screen;
    m_backdrop = std::move(backdrop);
    m_page = Page::Root;
    m_notice.clear();
    rebuild();
}
void SettingsMenu::close() {
    if (m_video.revert()) {
        m_confirmVideo = false;
        m_menu.close();
    }
}
std::vector<Extent2D> SettingsMenu::resolutions() const {
    auto result = m_display.resolutions;
    const Extent2D current{m_config.display.windowWidth, m_config.display.windowHeight};
    if (m_config.display.windowMode == WindowMode::Windowed &&
        std::ranges::find(result, current) == result.end()) {
        result.push_back(current);
    }
    std::ranges::sort(result, [](Extent2D a, Extent2D b) {
        return a.width == b.width ? a.height < b.height : a.width < b.width;
    });
    return result;
}
void SettingsMenu::rebuild(s32 selection) {
    if (m_queryDisplay) {
        m_display = m_queryDisplay();
    }
    auto definition = m_backdrop;
    definition.items.clear();
    definition.body.clear();
    definition.title = text(m_scope == Scope::Title ? "menu.options" : "pause.settings");
    definition.scale = 1.0f;
    definition.fades = m_backdrop.fades && m_page == Page::Root;
    const auto add = [&](std::string label, s32 code) {
        definition.items.push_back({std::move(label), code});
    };
    switch (m_page) {
    case Page::Root: {
        add(text("menu.audio"), 0);
        if (m_scope == Scope::Title) {
            add(text("menu.gameOptions"), 1);
        }
        if (m_scope != Scope::Level) {
            add(text("menu.compass"), 2);
        }
        add(text("menu.graphics"), kGraphicsCode);
        definition.items.push_back({text("menu.controls"), 3, 0, false});
        constexpr s32 kMargin = 64;
        const s32 left = definition.x < 0 ? kMargin : definition.x;
        s32 longest = 1;
        for (const auto& item : definition.items) {
            longest = std::max(longest, m_painter->measure(item.text, 1));
        }
        definition.scale =
            std::min(1.0f, static_cast<f32>(std::max(1, m_screen.width - left - kMargin)) /
                               static_cast<f32>(longest));
        definition.cursorScale = definition.scale;
        break;
    }
    case Page::Graphics: {
        definition.title = text("menu.graphics");
        definition.showCursor = false;
        definition.startSelects = false;
        constexpr s32 kLeft = 64;
        constexpr s32 kRight = 448;
        constexpr s32 kColumnGap = 28;
        constexpr s32 kRowTop = 122;
        constexpr s32 kRowStep = 26;
        constexpr s32 kActionY = 300;
        constexpr s32 kActionGap = 24;
        if (m_confirmVideo) {
            m_countdown = m_video.remaining();
            definition.body = {text("settings.savePrompt"),
                               text("settings.reverting") + " " + std::to_string(m_countdown) +
                                   " " +
                                   text(m_countdown == 1 ? "settings.second" : "settings.seconds")};
            definition.bodyY = 146;
            definition.bodyScale = 0.6f;
            definition.bodyGap = 14;
            add(text("settings.save"), 100);
            add(text("settings.revert"), 101);
            definition.scale = 0.7f;
        } else {
            const auto choice = [&](std::string label, std::string value, s32 code) {
                add(std::move(label), code);
                definition.items.back().value = std::move(value);
            };
            choice(text("settings.vsync"),
                   text(m_config.display.vsync ? "settings.on" : "settings.off"), 0);
            const auto rate = m_config.timing.gameplayFrameRate;
            choice(text("settings.fps"),
                   (rate == 0 ? text("settings.unlimited") : std::to_string(rate)), 1);
            const auto samples = m_config.display.sampleCount;
            std::string_view sampleLabel = "settings.off";
            if (samples == 2) {
                sampleLabel = "settings.msaa2";
            } else if (samples == 4) {
                sampleLabel = "settings.msaa4";
            }
            choice(text("settings.antialiasing"), text(sampleLabel), 2);
            const bool borderless = m_config.display.windowMode == WindowMode::BorderlessFullscreen;
            const auto size =
                borderless && !m_display.desktop.isZero()
                    ? m_display.desktop
                    : Extent2D{m_config.display.windowWidth, m_config.display.windowHeight};
            choice(text("settings.resolution"),
                   std::to_string(size.width) + " x " + std::to_string(size.height), 3);
            definition.items.back().enabled =
                m_config.display.windowMode == WindowMode::Fullscreen && resolutions().size() > 1;
            std::string_view modeLabel = "settings.windowed";
            if (m_config.display.windowMode == WindowMode::Fullscreen) {
                modeLabel = "settings.fullscreen";
            } else if (borderless) {
                modeLabel = "settings.borderless";
            }
            choice(text("settings.windowMode"), text(modeLabel), 4);
            definition.items.back().enabled = !m_display.desktop.isZero();
            choice(text("settings.depthOfField"),
                   text(m_config.display.depthOfField ? "settings.on" : "settings.off"), 8);
            s32 labelWidth = 0;
            s32 valueWidth = 0;
            for (const auto& item : definition.items) {
                labelWidth = std::max(labelWidth, m_painter->measure(item.text, 1));
                valueWidth = std::max(valueWidth, m_painter->measure(item.value, 1));
            }
            // Budget all choices, not just the current value, so cycling never resizes the font.
            for (const auto* id :
                 {"settings.borderless", "settings.fullscreen", "settings.windowed", "settings.on",
                  "settings.off", "settings.unlimited", "settings.msaa2", "settings.msaa4"}) {
                valueWidth = std::max(valueWidth, m_painter->measure(text(id), 1));
            }
            const auto measureSize = [&](Extent2D extent) {
                valueWidth =
                    std::max(valueWidth, m_painter->measure(std::to_string(extent.width) + " x " +
                                                                std::to_string(extent.height),
                                                            1));
            };
            for (const auto extent : m_display.resolutions) {
                measureSize(extent);
            }
            measureSize(m_display.desktop);
            const DisplayConfig defaults;
            measureSize({defaults.windowWidth, defaults.windowHeight});
            measureSize(
                {m_video.saved().display.windowWidth, m_video.saved().display.windowHeight});
            constexpr s32 kArrowMargin = 12;
            definition.scale =
                std::min(0.7f, static_cast<f32>(kRight - kLeft - kColumnGap - kArrowMargin) /
                                   static_cast<f32>(std::max(1, labelWidth + valueWidth)));
            const s32 actionWidth = m_painter->measure(text("settings.apply"), 1) +
                                    m_painter->measure(text("settings.defaults"), 1) +
                                    m_painter->measure(text("settings.back"), 1);
            definition.scale =
                std::min(definition.scale, static_cast<f32>(kRight - kLeft - 2 * kActionGap) /
                                               static_cast<f32>(std::max(1, actionWidth)));
            definition.valueX = kLeft +
                                static_cast<s32>(static_cast<f32>(labelWidth) * definition.scale) +
                                kColumnGap;
            definition.valueWidth =
                static_cast<s32>(static_cast<f32>(valueWidth) * definition.scale);
            for (usize i = 0; i < definition.items.size(); ++i) {
                definition.itemPositions.emplace_back(kLeft,
                                                      kRowTop + static_cast<s32>(i) * kRowStep);
            }
            add(text("settings.apply"), 5);
            add(text("settings.defaults"), 6);
            add(text("settings.back"), 7);
        }
        const usize firstAction = definition.itemPositions.size();
        s32 totalWidth = 0;
        for (usize i = firstAction; i < definition.items.size(); ++i) {
            totalWidth += m_painter->measure(definition.items[i].text, definition.scale);
            if (i > firstAction) {
                totalWidth += kActionGap;
            }
        }
        s32 x = (m_screen.width - totalWidth) / 2;
        for (usize i = firstAction; i < definition.items.size(); ++i) {
            definition.itemPositions.emplace_back(x, kActionY);
            x += m_painter->measure(definition.items[i].text, definition.scale) + kActionGap;
        }
        definition.cursorScale = definition.scale;
        break;
    }
    case Page::Audio: {
        definition.title = text("menu.audio");
        definition.x = 128;
        definition.y = 108;
        definition.scale = 0.8f;
        definition.selectLabel.clear();
        definition.items = {{text("settings.music"), 0, 52},
                            {text("settings.effects"), 1, 52},
                            {text("settings.mono"), 2, 0, true, text("settings.stereo"),
                             m_config.audio.stereo ? 2 : 1}};
        break;
    }
    case Page::Game:
        definition.title = text("menu.gameOptions");
        add(text("settings.difficulty"), 0);
        add(text("settings.multiplayer"), 1);
        break;
    case Page::Difficulty:
        definition.title = text("settings.difficulty");
        for (usize i = 0; i < DifficultyConfig::kNames.size(); ++i) {
            add(text("settings." + std::string(DifficultyConfig::kNames[i])), static_cast<s32>(i));
            definition.items.back().markedPart =
                m_config.difficulty.level == DifficultyConfig::kNames[i] ? 1 : 0;
        }
        break;
    case Page::Multiplayer: {
        definition.title = text("settings.multiplayer");
        s32 longest = 0;
        for (usize i = 0; i < MultiplayerConfig::kNames.size(); ++i) {
            add(text("settings.multiplayer." + std::string(MultiplayerConfig::kNames[i])),
                static_cast<s32>(i));
            definition.items.back().markedPart =
                static_cast<usize>(m_config.multiplayer.mode) == i ? 1 : 0;
            longest = std::max(longest, m_painter->measure(definition.items.back().text + " ~", 1));
        }
        constexpr f32 kMultiplayerScale = 0.65f;
        constexpr s32 kTextMargin = 64;
        const s32 left = definition.x < 0 ? kTextMargin : definition.x;
        const auto available = static_cast<f32>(std::max(1, m_screen.width - left - kTextMargin));
        definition.scale =
            std::min(kMultiplayerScale, available / static_cast<f32>(std::max(1, longest)));
        definition.cursorScale = definition.scale;
        break;
    }
    case Page::Compass:
        definition.title = text("menu.compass");
        add(text("settings.hide"), 0);
        add(text("settings.show"), 1);
        definition.items[m_config.camera.compass ? 1 : 0].markedPart = 1;
        break;
    }
    m_menu.open(definition, *m_painter, m_screen, selection);
}
bool SettingsMenu::flushAudio() {
    if (!m_audioDirty) {
        return true;
    }
    if (!m_persist || !m_persist(m_config)) {
        m_notice = text("settings.failed");
        return false;
    }
    m_audioDirty = false;
    m_notice.clear();
    return true;
}
void SettingsMenu::commit(GameConfig next) {
    if (m_persist && m_persist(next)) {
        m_config = std::move(next);
        m_notice.clear();
    } else {
        m_notice = text("settings.failed");
    }
    rebuild(m_menu.selection());
}
void SettingsMenu::change(s32 direction) {
    if (m_queryDisplay) {
        m_display = m_queryDisplay();
    }
    const s32 code = m_menu.definition().items[static_cast<usize>(m_menu.selection())].code;
    auto next = m_config;
    if (m_page == Page::Audio && code < 3) {
        if (code == 2) {
            next.audio.stereo = !next.audio.stereo;
        } else {
            auto& volume = code == 0 ? next.audio.musicVolume : next.audio.effectsVolume;
            volume =
                static_cast<f32>(std::clamp(AudioSlider::value(volume) + direction, 0, 255)) / 255;
        }
        m_config = std::move(next);
        m_menu.markItem(2, m_config.audio.stereo ? 2 : 1);
        m_audioDirty = true;
        m_notice.clear();
        if (m_previewAudio) {
            m_previewAudio(m_config.audio);
        }
        return;
    }
    if (m_page == Page::Graphics) {
        if (code == 0) {
            next.display.vsync = !next.display.vsync;
        } else if (code == 1) {
            const auto rate = cycleChoice(kFrameRates, next.timing.gameplayFrameRate, direction);
            next.display.maxFrameRate = rate;
            next.timing.gameplayFrameRate = rate;
        } else if (code == 2) {
            next.display.sampleCount =
                cycleChoice(kSampleCounts, next.display.sampleCount, direction);
        } else if (code == 3) {
            if (next.display.windowMode != WindowMode::Fullscreen) {
                return;
            }
            const auto choices = resolutions();
            if (choices.empty()) {
                return;
            }
            const auto it = std::ranges::find(
                choices, Extent2D{next.display.windowWidth, next.display.windowHeight});
            const auto count = static_cast<s32>(choices.size());
            const auto index = it == choices.end() ? 0 : static_cast<s32>(it - choices.begin());
            const auto size = choices[static_cast<usize>((index + direction + count) % count)];
            next.display.windowWidth = size.width;
            next.display.windowHeight = size.height;
        } else if (code == 8) {
            next.display.depthOfField = !next.display.depthOfField;
        } else if (code == 4) {
            if (m_display.desktop.isZero()) {
                return;
            }
            next.display.windowMode = static_cast<WindowMode>(
                (static_cast<s32>(next.display.windowMode) + direction + 3) % 3);
            if (next.display.windowMode == WindowMode::Fullscreen &&
                std::ranges::find(m_display.resolutions,
                                  Extent2D{next.display.windowWidth, next.display.windowHeight}) ==
                    m_display.resolutions.end()) {
                next.display.windowWidth = m_display.desktop.width;
                next.display.windowHeight = m_display.desktop.height;
            }
        }
        m_config = std::move(next);
        m_notice.clear();
        rebuild(m_menu.selection());
        return;
    }
    if (m_page == Page::Difficulty) {
        next.difficulty.level = DifficultyConfig::kNames[static_cast<usize>(code)];
    } else if (m_page == Page::Multiplayer) {
        next.multiplayer.mode = static_cast<MultiplayerMode>(code);
    } else if (m_page == Page::Compass) {
        next.camera.compass = code == 1;
    } else {
        return;
    }
    commit(std::move(next));
}
MenuEvent SettingsMenu::update(const MenuInput& input, s32 ticks) {
    if (!m_menu.isOpen()) {
        return {};
    }
    if (m_confirmVideo) {
        if (m_video.update()) {
            m_confirmVideo = false;
            m_config = m_video.saved();
            rebuild(kVideoRows);
            return {};
        }
        if (m_video.remaining() != m_countdown) {
            rebuild(m_menu.selection());
        }
        auto confirmInput = input;
        confirmInput.up |= input.left;
        confirmInput.down |= input.right;
        confirmInput.back |= input.escape;
        const auto event = m_menu.update(confirmInput, ticks);
        if (event.action == MenuAction::Back || event.action == MenuAction::Choice) {
            const bool save = event.action == MenuAction::Choice && event.code == 100;
            const bool success = save ? m_video.confirm() : m_video.revert();
            m_confirmVideo = m_video.pending();
            m_notice = success ? "" : text("settings.failed");
            if (!m_confirmVideo) {
                m_config = m_video.saved();
            }
            rebuild(m_confirmVideo ? 1 : kVideoRows);
        }
        return {};
    }
    const bool horizontal = input.left || input.right || input.leftHeld || input.rightHeld;
    if (m_page == Page::Graphics && !m_menu.closing() && (input.left || input.right)) {
        const auto selected = m_menu.selection();
        if (selected >= kVideoRows) {
            rebuild(kVideoRows + (selected - kVideoRows + (input.left ? -1 : 1) + 3) % 3);
        } else {
            change(input.left ? -1 : 1);
        }
        return {MenuAction::Moved, 0};
    }
    if (m_page == Page::Audio && !m_menu.closing()) {
        const bool stereo = m_menu.selection() == 2;
        if ((!stereo && horizontal && ticks > 0) || (stereo && (input.left || input.right))) {
            change((input.left || input.leftHeld ? -1 : 1) * (stereo ? 1 : ticks));
            return {MenuAction::Moved, 0};
        }
        if (!flushAudio() && (input.back || input.escape)) {
            return {};
        }
    }
    auto mapped = input;
    mapped.back |= input.escape;
    if (m_page == Page::Graphics && m_menu.selection() >= kVideoRows && (input.up || input.down)) {
        s32 row = kVideoRows - 1;
        while (row > 0 && !m_menu.definition().items[static_cast<usize>(row)].enabled) {
            --row;
        }
        rebuild(input.up ? row : 0);
        return {MenuAction::Moved, 0};
    }
    if (m_page == Page::Graphics && input.select) {
        const auto code = m_menu.definition().items[static_cast<usize>(m_menu.selection())].code;
        if (code == 5) {
            m_confirmVideo = m_video.apply(m_config);
            m_notice = m_confirmVideo ? "" : text("settings.failed");
            rebuild(m_confirmVideo ? 1 : kVideoRows);
        } else if (code == 6) {
            const GameConfig defaults;
            m_config.display.vsync = defaults.display.vsync;
            m_config.display.depthOfField = defaults.display.depthOfField;
            m_config.display.sampleCount = defaults.display.sampleCount;
            m_config.display.windowWidth = defaults.display.windowWidth;
            m_config.display.windowHeight = defaults.display.windowHeight;
            m_config.display.windowMode = defaults.display.windowMode;
            m_config.display.maxFrameRate = defaults.display.maxFrameRate;
            m_config.timing.gameplayFrameRate = defaults.timing.gameplayFrameRate;
            rebuild(kVideoRows + 1);
        } else if (code == 7) {
            mapped.select = false;
            mapped.back = true;
        }
        if (code != 7) {
            return {};
        }
    }
    const auto event = m_menu.update(mapped, ticks);
    if (event.action == MenuAction::Back) {
        if (m_page == Page::Graphics) {
            m_config = m_video.saved();
        }
        if (m_page == Page::Root) {
            return {MenuAction::Back, 0};
        }
        if (m_page == Page::Difficulty || m_page == Page::Multiplayer) {
            const s32 selection = m_page == Page::Multiplayer ? 1 : 0;
            m_page = Page::Game;
            m_notice.clear();
            rebuild(selection);
            return {};
        }
        const s32 previousCode =
            m_page == Page::Graphics ? kGraphicsCode : static_cast<s32>(m_page) - 1;
        m_page = Page::Root;
        m_notice.clear();
        rebuild();
        const auto& items = m_menu.definition().items;
        const auto previous = std::ranges::find(items, previousCode, &MenuItem::code);
        rebuild(static_cast<s32>(previous - items.begin()));
        return {};
    }
    if (event.action == MenuAction::Choice) {
        if (m_page == Page::Root) {
            m_page =
                event.code == kGraphicsCode ? Page::Graphics : static_cast<Page>(event.code + 1);
            if (m_page == Page::Graphics) {
                if (m_queryDisplay) {
                    m_display = m_queryDisplay();
                }
                if (m_config.display.windowMode == WindowMode::Windowed &&
                    !m_display.window.isZero()) {
                    m_config.display.windowWidth = m_display.window.width;
                    m_config.display.windowHeight = m_display.window.height;
                }
                m_video.begin(m_config, m_previewVideo, m_persist, m_clock);
            }
            m_notice.clear();
            rebuild(m_page == Page::Compass && m_config.camera.compass ? 1 : 0);
        } else if (m_page == Page::Game && event.code == 0) {
            m_page = Page::Difficulty;
            // MSVC's checked array iterator is not a pointer; keep the portable iterator type.
            // NOLINTNEXTLINE(readability-qualified-auto)
            const auto selected =
                std::ranges::find(DifficultyConfig::kNames, m_config.difficulty.level);
            rebuild(selected == DifficultyConfig::kNames.end()
                        ? 1
                        : static_cast<s32>(selected - DifficultyConfig::kNames.begin()));
        } else if (m_page == Page::Game && event.code == 1) {
            m_page = Page::Multiplayer;
            rebuild(static_cast<s32>(m_config.multiplayer.mode));
        } else if (m_page == Page::Audio) {
            // Confirm selects the row but does not alter its setting.
        } else {
            change(1);
        }
    }
    return event;
}
void SettingsMenu::draw(Canvas& canvas, const TextPainter& painter,
                        const MenuTextures& textures) const {
    if (!m_menu.isOpen()) {
        return;
    }
    m_menu.draw(canvas, painter, textures);
    if (m_page == Page::Audio) {
        const std::array volumes{m_config.audio.musicVolume, m_config.audio.effectsVolume};
        for (usize i = 0; i < volumes.size(); ++i) {
            textures.audioSlider.draw(
                canvas, 128, static_cast<f32>(m_menu.itemY(i) + m_menu.lineHeight()), volumes[i],
                m_menu.selection() == static_cast<s32>(i), m_menu.fadeOpacity());
        }
    }
    TextStyle style;
    style.scale = 0.5f;
    style.color = m_menu.definition().colors.off;
    if (!m_notice.empty()) {
        painter.draw(canvas, -m_screen.width / 2, m_page == Page::Audio ? 348 : 276, m_notice,
                     style);
    }
}
} // namespace gdl::game
