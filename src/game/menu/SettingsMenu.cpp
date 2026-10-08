#include "game/menu/SettingsMenu.h"

#include <algorithm>
#include <array>
#include <span>
#include <utility>

#include "engine/core/Types.h"

namespace gdl::game {
namespace {
constexpr s32 kGraphicsCode = 4;
constexpr s32 kVideoRows = 9;
constexpr s32 kAudioSliders = 3;
constexpr s32 kAudioX = 160;
constexpr f32 kAudioScale = 0.7f;
constexpr std::array<u32, 3> kFrameRates{30, 60, 0};
constexpr std::array<u32, 3> kSampleCounts{1, 2, 4};
constexpr std::array<u32, 6> kTextureFiltering{0, 1, 2, 4, 8, 16};
constexpr std::array<std::string_view, 6> kTextureLabels{
    "settings.off",          "settings.trilinear",    "settings.anisotropic2",
    "settings.anisotropic4", "settings.anisotropic8", "settings.anisotropic16"};

u32 cycleChoice(std::span<const u32> choices, u32 value, s32 direction) {
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
    m_audioSampleTicks = 0;
    m_audioSampleMagic = false;
    m_audioSample = {};
    m_audioDrag.reset();
    m_audioPointer.reset();
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
    case Page::Controls:
        m_controls.define(definition, *m_painter, [this](std::string_view id) { return text(id); });
        break;
    case Page::Root: {
        add(text("menu.audio"), 0);
        add(text("menu.gameOptions"), 1);
        if (m_scope != Scope::Level) {
            add(text("menu.compass"), 2);
        }
        add(text("menu.graphics"), kGraphicsCode);
        definition.items.push_back({text("menu.controls"), 3});
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
        constexpr s32 kRowTop = 100;
        constexpr s32 kRowStep = 19;
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
            choice(text("settings.bloom"),
                   text(m_config.display.bloom ? "settings.on" : "settings.off"), 9);
            choice(text("settings.ambientOcclusion"),
                   text(m_config.display.ambientOcclusion ? "settings.on" : "settings.off"), 10);
            const auto filter =
                std::ranges::find(kTextureFiltering, m_config.display.textureFiltering);
            const auto filterIndex = filter == kTextureFiltering.end()
                                         ? usize{1}
                                         : static_cast<usize>(filter - kTextureFiltering.begin());
            choice(text("settings.textureFiltering"), text(kTextureLabels[filterIndex]), 11);
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
            for (const auto id : kTextureLabels) {
                valueWidth = std::max(valueWidth, m_painter->measure(text(id), 1));
            }
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
        definition.x = kAudioX;
        definition.y = 96;
        definition.scale = kAudioScale;
        definition.cursorScale = kAudioScale;
        definition.selectLabel.clear();
        definition.items = {{text("settings.music"), 0},
                            {text("settings.effects"), 1},
                            {text("settings.movies"), 2},
                            {text("settings.mono"), kAudioSliders, 0, true, text("settings.stereo"),
                             m_config.audio.stereo ? 2 : 1}};
        constexpr s32 kAudioRowStep = 70;
        for (usize i = 0; i < definition.items.size(); ++i) {
            definition.itemPositions.emplace_back(kAudioX, definition.y +
                                                               static_cast<s32>(i) * kAudioRowStep);
        }
        break;
    }
    case Page::Game:
        definition.title = text("menu.gameOptions");
        add(text("settings.difficulty"), 0);
        add(text("settings.multiplayer"), 1);
        add(text("settings.combat"), 2);
        break;
    case Page::Combat:
        definition.title = text("settings.combat");
        definition.x = 96;
        definition.y = 160;
        definition.scale = 0.7f;
        definition.cursorScale = definition.scale;
        definition.valueX = 344;
        definition.valueWidth =
            std::max(m_painter->measure(text("settings.on"), definition.scale),
                     m_painter->measure(text("settings.off"), definition.scale));
        add(text("settings.autoMelee"), 0);
        definition.items.back().value =
            text(m_config.combat.autoMelee ? "settings.on" : "settings.off");
        add(text("settings.enemyHealthBars"), 1);
        definition.items.back().value =
            text(m_config.combat.enemyHealthBars ? "settings.on" : "settings.off");
        add(text("settings.autoActivateItems"), 2);
        definition.items.back().value =
            text(m_config.combat.autoActivateItems ? "settings.on" : "settings.off");
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
f32& SettingsMenu::audioVolume(usize row) {
    if (row == 0) {
        return m_config.audio.musicVolume;
    }
    return row == 1 ? m_config.audio.effectsVolume : m_config.audio.movieVolume;
}
void SettingsMenu::change(s32 direction) {
    if (m_queryDisplay) {
        m_display = m_queryDisplay();
    }
    const s32 code = m_menu.definition().items[static_cast<usize>(m_menu.selection())].code;
    auto next = m_config;
    if (m_page == Page::Audio) {
        if (code == kAudioSliders) {
            m_config.audio.stereo = !m_config.audio.stereo;
        } else {
            auto& volume = audioVolume(static_cast<usize>(code));
            const auto previous = AudioSlider::value(volume);
            volume = static_cast<f32>(std::clamp(previous + direction, 0, 255)) / 255;
            if (code == 1 && AudioSlider::value(volume) == previous && m_audioSampleTicks > 15) {
                m_audioSample = "S_VOLMOVE";
                m_audioSampleTicks = 0;
            }
        }
        m_menu.markItem(kAudioSliders, m_config.audio.stereo ? 2 : 1);
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
        } else if (code == 9) {
            next.display.bloom = !next.display.bloom;
        } else if (code == 10) {
            next.display.ambientOcclusion = !next.display.ambientOcclusion;
        } else if (code == 11) {
            next.display.textureFiltering =
                cycleChoice(kTextureFiltering, next.display.textureFiltering, direction);
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
    } else if (m_page == Page::Combat) {
        switch (code) {
        case 0: next.combat.autoMelee = !next.combat.autoMelee; break;
        case 1: next.combat.enemyHealthBars = !next.combat.enemyHealthBars; break;
        case 2: next.combat.autoActivateItems = !next.combat.autoActivateItems; break;
        default: return;
        }
    } else {
        return;
    }
    commit(std::move(next));
}
MenuEvent SettingsMenu::update(const MenuInput& input, s32 ticks) {
    m_audioSample = {};
    if (!m_menu.isOpen()) {
        return {};
    }
    if (m_page == Page::Controls) {
        if (m_controls.update(input, ticks, m_menu, m_config, m_persist,
                              [this](s32 selected) { rebuild(selected); })) {
            m_page = Page::Root;
            rebuild();
        }
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
    if (m_page == Page::Combat && !m_menu.closing() && (input.left || input.right)) {
        change(input.left ? -1 : 1);
        return {MenuAction::Moved, 0};
    }
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
        m_audioSampleTicks = std::min(61, m_audioSampleTicks + std::max(0, ticks));
        if (input.pointer && !input.pointerNormalized) {
            for (usize i = 0; i < kAudioSliders; ++i) {
                const auto area = AudioSlider::track(
                    kAudioX, static_cast<f32>(m_menu.itemY(i) + m_menu.lineHeight()), kAudioScale);
                const bool inside =
                    input.pointer->x >= area.x && input.pointer->x <= area.x + area.width &&
                    input.pointer->y >= area.y && input.pointer->y < area.y + area.height;
                if (inside && (m_audioPointer != input.pointer || input.pointerPressed)) {
                    m_menu.focus(i);
                }
                if (inside && input.pointerPressed) {
                    m_audioDrag = i;
                }
            }
            m_audioPointer = input.pointer;
            if (m_audioDrag && (input.pointerHeld || input.pointerPressed)) {
                m_menu.focus(*m_audioDrag);
                const auto volume = AudioSlider::volumeAt(input.pointer->x, kAudioX, kAudioScale);
                const auto current = audioVolume(*m_audioDrag);
                change(AudioSlider::value(volume) - AudioSlider::value(current));
                return {};
            }
        } else {
            m_audioPointer.reset();
        }
        if (!input.pointerHeld) {
            m_audioDrag.reset();
        }
        const bool stereo = m_menu.selection() == kAudioSliders;
        if ((!stereo && horizontal && ticks > 0) || (stereo && (input.left || input.right))) {
            change((input.left || input.leftHeld ? -1 : 1) * (stereo ? 1 : ticks));
            return stereo ? MenuEvent{MenuAction::Moved, 0} : MenuEvent{};
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
    auto event = m_menu.update(mapped, ticks);
    if (m_page == Page::Graphics && event.action == MenuAction::Choice) {
        const auto code = event.code;
        if (code == 5) {
            m_confirmVideo = m_video.apply(m_config);
            m_notice = m_confirmVideo ? "" : text("settings.failed");
            rebuild(m_confirmVideo ? 1 : kVideoRows);
        } else if (code == 6) {
            const GameConfig defaults;
            m_config.display.vsync = defaults.display.vsync;
            m_config.display.depthOfField = defaults.display.depthOfField;
            m_config.display.bloom = defaults.display.bloom;
            m_config.display.ambientOcclusion = defaults.display.ambientOcclusion;
            m_config.display.sampleCount = defaults.display.sampleCount;
            m_config.display.textureFiltering = defaults.display.textureFiltering;
            m_config.display.windowWidth = defaults.display.windowWidth;
            m_config.display.windowHeight = defaults.display.windowHeight;
            m_config.display.windowMode = defaults.display.windowMode;
            m_config.display.maxFrameRate = defaults.display.maxFrameRate;
            m_config.timing.gameplayFrameRate = defaults.timing.gameplayFrameRate;
            rebuild(kVideoRows + 1);
        } else if (code == 7) {
            event.action = MenuAction::Back;
        } else if (event.direction != 0) {
            change(event.direction);
        }
        if (code != 7) {
            return {};
        }
    }
    if (event.action == MenuAction::Back) {
        if (m_page == Page::Graphics) {
            m_config = m_video.saved();
        }
        if (m_page == Page::Root) {
            return {MenuAction::Back, 0};
        }
        if (m_page == Page::Difficulty || m_page == Page::Multiplayer || m_page == Page::Combat) {
            s32 selection = 0;
            if (m_page == Page::Multiplayer) {
                selection = 1;
            } else if (m_page == Page::Combat) {
                selection = 2;
            }
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
            if (event.code == 3) {
                m_page = Page::Controls;
                m_controls.begin(m_config);
                rebuild();
                return {};
            }
            m_page =
                event.code == kGraphicsCode ? Page::Graphics : static_cast<Page>(event.code + 1);
            if (m_page == Page::Audio) {
                m_audioSampleTicks = 0;
            }
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
        } else if (m_page == Page::Game && event.code == 2) {
            m_page = Page::Combat;
            rebuild();
        } else if (m_page == Page::Audio) {
            if (event.code == kAudioSliders && event.part != 0 &&
                m_config.audio.stereo != (event.part == 2)) {
                change(1);
                flushAudio();
            }
        } else {
            change(1);
        }
    }
    if (m_page == Page::Audio && m_menu.selection() == 1 && m_audioSampleTicks > 60 &&
        (event.action == MenuAction::None || event.action == MenuAction::Moved)) {
        m_audioSample = m_audioSampleMagic ? "S_PICKUPMAGIC" : "S_WARN";
        m_audioSampleMagic = !m_audioSampleMagic;
        m_audioSampleTicks = 0;
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
        const std::array volumes{m_config.audio.musicVolume, m_config.audio.effectsVolume,
                                 m_config.audio.movieVolume};
        for (usize i = 0; i < volumes.size(); ++i) {
            textures.audioSlider.draw(canvas, kAudioX,
                                      static_cast<f32>(m_menu.itemY(i) + m_menu.lineHeight()),
                                      volumes[i], m_menu.selection() == static_cast<s32>(i),
                                      m_menu.fadeOpacity(), kAudioScale);
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
