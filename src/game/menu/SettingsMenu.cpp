#include "game/menu/SettingsMenu.h"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "engine/core/Types.h"

#include "game/menu/SettingsRows.h"

namespace gdl::game {
namespace {
constexpr s32 kGraphicsCode = 4;
constexpr s32 kLanguageCode = 5;
constexpr s32 kAggressorModeCode = 6;
constexpr s32 kDisplayCode = 13;
constexpr s32 kRendererCode = 14;
constexpr s32 kAudioSliders = 3;
/** Where a notice stands under the Audio sliders and the Video action row. */
constexpr s32 kLowNoticeY = 348;
constexpr s32 kNoticeY = 276;
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

constexpr std::array<std::string_view, 2> kOnOff{"settings.on", "settings.off"};
constexpr std::array<std::string_view, 2> kRendererLabels{"settings.vulkan", "settings.opengl"};
constexpr std::array<std::string_view, 1> kFrameRateLabels{"settings.unlimited"};
constexpr std::array<std::string_view, 3> kSampleLabels{"settings.off", "settings.msaa2",
                                                        "settings.msaa4"};
constexpr std::array<std::string_view, 3> kWindowModeLabels{
    "settings.windowed", "settings.fullscreen", "settings.borderless"};

using Page = SettingsMenu::Page;
using Scope = SettingsMenu::Scope;

/** A row that opens another page. */
struct PageLink {
    s32 code = 0;
    std::string_view label;
    Page page = Page::Root;
};
/** The root's pages, in the order listed; the compass is not offered in a level. */
constexpr std::array<PageLink, 5> kRootLinks{{{0, "menu.audio", Page::Audio},
                                              {1, "menu.gameOptions", Page::Game},
                                              {2, "menu.compass", Page::Compass},
                                              {kGraphicsCode, "menu.graphics", Page::Graphics},
                                              {3, "menu.controls", Page::Controls}}};
/** The Game page's own pages: its first rows, each code its place. */
constexpr std::array<PageLink, 3> kGameLinks{{{0, "settings.difficulty", Page::Difficulty},
                                              {1, "settings.multiplayer", Page::Multiplayer},
                                              {2, "settings.combat", Page::Combat}}};

const PageLink* linkOf(std::span<const PageLink> links, s32 code) {
    for (const PageLink& link : links) {
        if (link.code == code) {
            return &link;
        }
    }
    return nullptr;
}
const PageLink* linkTo(std::span<const PageLink> links, Page page) {
    for (const PageLink& link : links) {
        if (link.page == page) {
            return &link;
        }
    }
    return nullptr;
}

/** An On/Off row bound to its setting. */
template <class Config> struct Switch {
    s32 code = 0;
    std::string_view label;
    bool Config::*field = nullptr;
};
constexpr std::array<Switch<GameOptionsConfig>, 3> kGameSwitches{
    {{3, "settings.expandedShop", &GameOptionsConfig::expandedShop},
     {4, "settings.autoSave", &GameOptionsConfig::autoSave},
     {kAggressorModeCode, "settings.aggressorMode", &GameOptionsConfig::aggressorMode}}};
constexpr std::array<Switch<CombatConfig>, 4> kCombatSwitches{
    {{0, "settings.autoMelee", &CombatConfig::autoMelee},
     {1, "settings.enemyHealthBars", &CombatConfig::enemyHealthBars},
     {2, "settings.autoActivateItems", &CombatConfig::autoActivateItems},
     {3, "settings.autoAim", &CombatConfig::autoAim}}};

/** Flips the switch with `code`; false when there is none. */
template <class Config, usize N>
bool flipSwitch(const std::array<Switch<Config>, N>& switches, s32 code, Config& config) {
    for (const auto& entry : switches) {
        if (entry.code == code) {
            config.*entry.field = !(config.*entry.field);
            return true;
        }
    }
    return false;
}

/** What the Video rows read besides the settings: the scope and what the machine offers. */
struct VideoView {
    Scope scope = Scope::Title;
    const DisplayOptions& display;
    std::vector<Extent2D> resolutions; ///< the sizes a fullscreen window may take
    s32 monitor = 0;                   ///< the display shown: the one chosen, else the window's
};

/** What a row shows: a text id to look up, or text shown as it is (a size, a count). */
struct Shown {
    std::string_view id;
    std::string literal;
};

std::string sizeText(Extent2D size) {
    return std::to_string(size.width) + " x " + std::to_string(size.height);
}

/**
 * One Video row, defined once: its code and label, what it shows, when it is offered, what a
 * step does to it and what Restore Defaults puts back. An On/Off row names its setting and
 * needs nothing more; `enabled` alone greys a row, a step keeps its own guard.
 */
struct VideoRow {
    s32 code = 0;
    std::string_view label;
    bool DisplayConfig::*toggle = nullptr;
    Shown (*shown)(const GameConfig&, const VideoView&) = nullptr;
    bool (*enabled)(const GameConfig&, const VideoView&) = nullptr; ///< none: always offered
    /** Steps the row by `direction`; false leaves the settings and the page as they were. */
    bool (*step)(GameConfig&, const VideoView&, s32 direction) = nullptr;
    /** What Restore Defaults puts back; none: the row keeps its value. */
    void (*restore)(GameConfig&, const GameConfig& defaults, const VideoView&) = nullptr;
    std::span<const std::string_view> labels; ///< every text id it may show, for the width
};

/** Scene post-processing is the Vulkan device's alone. */
bool vulkanOnly(const GameConfig& config, const VideoView& /*view*/) {
    return config.display.renderer != GraphicsApi::OpenGl;
}

const std::array<VideoRow, 12> kVideoTable{{
    // The renderer is chosen from the title alone, the one drawing a level not being
    // swappable under it; the choice takes effect when the game is next started.
    {.code = kRendererCode,
     .label = "settings.renderer",
     .shown =
         [](const GameConfig& config, const VideoView& /*view*/) {
             return Shown{config.display.renderer == GraphicsApi::OpenGl ? "settings.opengl"
                                                                         : "settings.vulkan"};
         },
     .enabled = [](const GameConfig& /*config*/,
                   const VideoView& view) { return view.scope == Scope::Title; },
     .step =
         [](GameConfig& config, const VideoView& view, s32 /*direction*/) {
             if (view.scope != Scope::Title) {
                 return false;
             }
             config.display.renderer = otherGraphicsApi(config.display.renderer);
             return true;
         },
     .restore =
         [](GameConfig& config, const GameConfig& defaults, const VideoView& view) {
             if (view.scope == Scope::Title) {
                 config.display.renderer = defaults.display.renderer;
             }
         },
     .labels = kRendererLabels},
    {.code = 0, .label = "settings.vsync", .toggle = &DisplayConfig::vsync},
    {.code = 1,
     .label = "settings.fps",
     .shown =
         [](const GameConfig& config, const VideoView& /*view*/) {
             const auto rate = config.timing.gameplayFrameRate;
             return rate == 0 ? Shown{"settings.unlimited"} : Shown{{}, std::to_string(rate)};
         },
     .step =
         [](GameConfig& config, const VideoView& /*view*/, s32 direction) {
             const auto rate = cycleChoice(kFrameRates, config.timing.gameplayFrameRate, direction);
             config.display.maxFrameRate = rate;
             config.timing.gameplayFrameRate = rate;
             return true;
         },
     .restore =
         [](GameConfig& config, const GameConfig& defaults, const VideoView& /*view*/) {
             config.display.maxFrameRate = defaults.display.maxFrameRate;
             config.timing.gameplayFrameRate = defaults.timing.gameplayFrameRate;
         },
     .labels = kFrameRateLabels},
    {.code = 2,
     .label = "settings.antialiasing",
     .shown =
         [](const GameConfig& config, const VideoView& /*view*/) {
             const auto samples = config.display.sampleCount;
             return Shown{samples == 2   ? "settings.msaa2"
                          : samples == 4 ? "settings.msaa4"
                                         : "settings.off"};
         },
     .step =
         [](GameConfig& config, const VideoView& /*view*/, s32 direction) {
             config.display.sampleCount =
                 cycleChoice(kSampleCounts, config.display.sampleCount, direction);
             return true;
         },
     .restore =
         [](GameConfig& config, const GameConfig& defaults, const VideoView& /*view*/) {
             config.display.sampleCount = defaults.display.sampleCount;
         },
     .labels = kSampleLabels},
    // Chosen in fullscreen alone; borderless shows the desktop's size.
    {.code = 3,
     .label = "settings.resolution",
     .shown =
         [](const GameConfig& config, const VideoView& view) {
             const bool borderless = config.display.windowMode == WindowMode::BorderlessFullscreen;
             return Shown{
                 {},
                 sizeText(borderless && !view.display.desktop.isZero()
                              ? view.display.desktop
                              : Extent2D{config.display.windowWidth, config.display.windowHeight})};
         },
     .enabled =
         [](const GameConfig& config, const VideoView& view) {
             return config.display.windowMode == WindowMode::Fullscreen &&
                    view.resolutions.size() > 1;
         },
     .step =
         [](GameConfig& config, const VideoView& view, s32 direction) {
             if (config.display.windowMode != WindowMode::Fullscreen || view.resolutions.empty()) {
                 return false;
             }
             const auto& choices = view.resolutions;
             const auto it = std::ranges::find(
                 choices, Extent2D{config.display.windowWidth, config.display.windowHeight});
             const auto count = static_cast<s32>(choices.size());
             const auto index = it == choices.end() ? 0 : static_cast<s32>(it - choices.begin());
             const auto size = choices[static_cast<usize>((index + direction + count) % count)];
             config.display.windowWidth = size.width;
             config.display.windowHeight = size.height;
             return true;
         },
     .restore =
         [](GameConfig& config, const GameConfig& defaults, const VideoView& /*view*/) {
             config.display.windowWidth = defaults.display.windowWidth;
             config.display.windowHeight = defaults.display.windowHeight;
         }},
    // Going fullscreen at a size the display does not offer takes the desktop's.
    {.code = 4,
     .label = "settings.windowMode",
     .shown =
         [](const GameConfig& config, const VideoView& /*view*/) {
             const auto mode = config.display.windowMode;
             return Shown{mode == WindowMode::Fullscreen             ? "settings.fullscreen"
                          : mode == WindowMode::BorderlessFullscreen ? "settings.borderless"
                                                                     : "settings.windowed"};
         },
     .enabled = [](const GameConfig& /*config*/,
                   const VideoView& view) { return !view.display.desktop.isZero(); },
     .step =
         [](GameConfig& config, const VideoView& view, s32 direction) {
             if (view.display.desktop.isZero()) {
                 return false;
             }
             config.display.windowMode = static_cast<WindowMode>(
                 (static_cast<s32>(config.display.windowMode) + direction + 3) % 3);
             if (config.display.windowMode == WindowMode::Fullscreen &&
                 std::ranges::find(
                     view.display.resolutions,
                     Extent2D{config.display.windowWidth, config.display.windowHeight}) ==
                     view.display.resolutions.end()) {
                 config.display.windowWidth = view.display.desktop.width;
                 config.display.windowHeight = view.display.desktop.height;
             }
             return true;
         },
     .restore =
         [](GameConfig& config, const GameConfig& defaults, const VideoView& /*view*/) {
             config.display.windowMode = defaults.display.windowMode;
         },
     .labels = kWindowModeLabels},
    // The display, counted as the platform lists them; one alone offers no choice. Restore
    // Defaults leaves it where it is.
    {.code = kDisplayCode,
     .label = "settings.display",
     .shown = [](const GameConfig& /*config*/,
                 const VideoView& view) { return Shown{{}, std::to_string(view.monitor + 1)}; },
     .enabled = [](const GameConfig& /*config*/,
                   const VideoView& view) { return view.display.monitors.size() > 1; },
     .step =
         [](GameConfig& config, const VideoView& view, s32 direction) {
             const auto count = static_cast<s32>(view.display.monitors.size());
             if (count < 2) {
                 return false;
             }
             config.display.monitor = (view.monitor + direction + count) % count;
             return true;
         }},
    {.code = 8,
     .label = "settings.depthOfField",
     .toggle = &DisplayConfig::depthOfField,
     .enabled = vulkanOnly},
    {.code = 9, .label = "settings.bloom", .toggle = &DisplayConfig::bloom, .enabled = vulkanOnly},
    {.code = 10,
     .label = "settings.ambientOcclusion",
     .toggle = &DisplayConfig::ambientOcclusion,
     .enabled = vulkanOnly},
    {.code = 11,
     .label = "settings.textureFiltering",
     .shown =
         [](const GameConfig& config, const VideoView& /*view*/) {
             const auto index = static_cast<usize>(
                 std::ranges::find(kTextureFiltering, config.display.textureFiltering) -
                 kTextureFiltering.begin());
             return Shown{kTextureLabels[index < kTextureLabels.size() ? index : 1]};
         },
     .step =
         [](GameConfig& config, const VideoView& /*view*/, s32 direction) {
             config.display.textureFiltering =
                 cycleChoice(kTextureFiltering, config.display.textureFiltering, direction);
             return true;
         },
     .restore =
         [](GameConfig& config, const GameConfig& defaults, const VideoView& /*view*/) {
             config.display.textureFiltering = defaults.display.textureFiltering;
         },
     .labels = kTextureLabels},
    {.code = 12, .label = "settings.smoothSprites", .toggle = &DisplayConfig::smoothSprites},
}};
constexpr s32 kVideoRows = static_cast<s32>(std::tuple_size_v<decltype(kVideoTable)>);

const VideoRow* videoRow(s32 code) {
    for (const VideoRow& row : kVideoTable) {
        if (row.code == code) {
            return &row;
        }
    }
    return nullptr;
}
Shown shownOf(const VideoRow& row, const GameConfig& config, const VideoView& view) {
    if (row.toggle != nullptr) {
        return Shown{config.display.*row.toggle ? "settings.on" : "settings.off"};
    }
    return row.shown(config, view);
}
bool stepRow(const VideoRow& row, GameConfig& config, const VideoView& view, s32 direction) {
    if (row.toggle != nullptr) {
        config.display.*row.toggle = !(config.display.*row.toggle);
        return true;
    }
    return row.step(config, view, direction);
}
void restoreRow(const VideoRow& row, GameConfig& config, const GameConfig& defaults,
                const VideoView& view) {
    if (row.toggle != nullptr) {
        config.display.*row.toggle = defaults.display.*row.toggle;
    } else if (row.restore != nullptr) {
        row.restore(config, defaults, view);
    }
}
} // namespace
std::string SettingsMenu::text(std::string_view id) const {
    return std::string(m_strings != nullptr ? m_strings->get(id) : id);
}
std::vector<StringTable::Language> SettingsMenu::languages() const {
    return m_strings != nullptr ? m_strings->languages() : std::vector<StringTable::Language>{};
}
std::string SettingsMenu::languageName() const {
    for (const auto& language : languages()) {
        if (language.code == m_config.text.language) {
            return language.name;
        }
    }
    return m_config.text.language;
}
void SettingsMenu::open(const GameConfig& config, const StringTable* strings, Persist persist,
                        const TextPainter& painter, const MenuScreen& screen,
                        MenuDefinition backdrop, Scope scope, PreviewAudio preview,
                        DisplayOptions display, std::function<DisplayOptions()> queryDisplay,
                        Persist previewVideo, VideoSettings::Clock clock, GraphicsApi renderer) {
    if (!m_video.revert()) {
        return;
    }
    m_config = config;
    m_renderer = renderer;
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
s32 SettingsMenu::shownMonitor() const {
    const auto count = static_cast<s32>(m_display.monitors.size());
    if (m_config.display.monitor >= 0 && m_config.display.monitor < count) {
        return m_config.display.monitor;
    }
    return std::clamp(m_display.monitor, 0, std::max(0, count - 1));
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
    SettingsRows rows(definition, *m_painter, [this](std::string_view id) { return text(id); });
    const auto add = [&](std::string label, s32 code) { rows.choice(std::move(label), code); };
    switch (m_page) {
    case Page::Controls:
        m_controls.define(definition, *m_painter, [this](std::string_view id) { return text(id); });
        break;
    case Page::Root: {
        for (const PageLink& link : kRootLinks) {
            if (link.page != Page::Compass || m_scope != Scope::Level) {
                add(text(link.label), link.code);
            }
        }
        rows.fit(1.0f, m_screen.width);
        break;
    }
    case Page::Graphics: {
        definition.title = text("menu.graphics");
        definition.showCursor = false;
        definition.startSelects = false;
        constexpr s32 kLeft = 64;
        constexpr s32 kRight = 448;
        constexpr s32 kColumnGap = 28;
        constexpr s32 kRowTop = 104; ///< under the title's cell; twelve rows to the actions
        constexpr s32 kRowStep = 16;
        constexpr s32 kActionY = 316;
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
            const VideoView view{m_scope, m_display, resolutions(), shownMonitor()};
            for (const VideoRow& row : kVideoTable) {
                const Shown shown = shownOf(row, m_config, view);
                rows.value(text(row.label), row.code,
                           shown.id.empty() ? shown.literal : text(shown.id),
                           row.enabled == nullptr || row.enabled(m_config, view));
            }
            s32 labelWidth = 0;
            s32 valueWidth = 0;
            for (const auto& item : definition.items) {
                labelWidth = std::max(labelWidth, m_painter->measure(item.text, 1));
                valueWidth = std::max(valueWidth, m_painter->measure(item.value, 1));
            }
            // Budget all choices, not just the current value, so cycling never resizes the font.
            for (const VideoRow& row : kVideoTable) {
                for (const auto id : row.toggle != nullptr
                                         ? std::span<const std::string_view>{kOnOff}
                                         : row.labels) {
                    valueWidth = std::max(valueWidth, m_painter->measure(text(id), 1));
                }
            }
            const auto measureSize = [&](Extent2D extent) {
                valueWidth = std::max(valueWidth, m_painter->measure(sizeText(extent), 1));
            };
            valueWidth = std::max(valueWidth,
                                  m_painter->measure(std::to_string(m_display.monitors.size()), 1));
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
                                    m_painter->measure(text("settings.defaults"), 1);
            definition.scale =
                std::min(definition.scale, static_cast<f32>(kRight - kLeft - kActionGap) /
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
        rows.column();
        rows.switches();
        for (const PageLink& link : kGameLinks) {
            add(text(link.label), link.code);
        }
        for (const auto& entry : kGameSwitches) {
            rows.toggle(text(entry.label), entry.code, m_config.game.*entry.field);
        }
        rows.value(text("settings.language"), kLanguageCode, languageName());
        break;
    case Page::Combat:
        definition.title = text("settings.combat");
        rows.column(SettingsRows::kColumnX, 160);
        rows.switches();
        for (const auto& entry : kCombatSwitches) {
            rows.toggle(text(entry.label), entry.code, m_config.combat.*entry.field);
        }
        break;
    case Page::Difficulty:
        definition.title = text("settings.difficulty");
        for (usize i = 0; i < DifficultyConfig::kNames.size(); ++i) {
            rows.marked(text("settings." + std::string(DifficultyConfig::kNames[i])),
                        static_cast<s32>(i),
                        m_config.difficulty.level == DifficultyConfig::kNames[i]);
        }
        break;
    case Page::Multiplayer: {
        definition.title = text("settings.multiplayer");
        for (usize i = 0; i < MultiplayerConfig::kNames.size(); ++i) {
            rows.marked(text("settings.multiplayer." + std::string(MultiplayerConfig::kNames[i])),
                        static_cast<s32>(i), static_cast<usize>(m_config.multiplayer.mode) == i);
        }
        // The mark beside a choice takes the room of a tilde.
        constexpr f32 kMultiplayerScale = 0.65f;
        rows.fit(kMultiplayerScale, m_screen.width, " ~");
        break;
    }
    case Page::Compass:
        definition.title = text("menu.compass");
        rows.marked(text("settings.hide"), 0, !m_config.camera.compass);
        rows.marked(text("settings.show"), 1, m_config.camera.compass);
        break;
    }
    // A page written again (a row toggled, a key captured) keeps its window where it was;
    // another page, or another player's controls, starts at the top.
    const s32 windowKey =
        static_cast<s32>(m_page) * 8 + (m_page == Page::Controls ? m_controls.player() + 1 : 0);
    const s32 scroll = m_menu.isOpen() && windowKey == m_windowKey ? m_menu.scroll() : 0;
    m_windowKey = windowKey;
    m_menu.open(definition, *m_painter, m_screen, selection, scroll);
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
        const VideoView view{m_scope, m_display, resolutions(), shownMonitor()};
        if (const VideoRow* row = videoRow(code);
            row != nullptr && !stepRow(*row, next, view, direction)) {
            return;
        }
        m_config = std::move(next);
        m_notice.clear();
        rebuild(m_menu.selection());
        return;
    }
    if (m_page == Page::Game) {
        if (code == kLanguageCode) {
            // The next language the text directory holds; a lone one stays.
            const auto choices = languages();
            if (choices.empty()) {
                return;
            }
            const auto current =
                std::ranges::find(choices, next.text.language, &StringTable::Language::code);
            const auto count = static_cast<s32>(choices.size());
            const s32 index =
                current == choices.end() ? 0 : static_cast<s32>(current - choices.begin());
            next.text.language =
                choices[static_cast<usize>((index + direction + count) % count)].code;
            if (next.text.language == m_config.text.language) {
                return;
            }
        } else if (!flipSwitch(kGameSwitches, code, next.game)) {
            return;
        }
    } else if (m_page == Page::Difficulty) {
        next.difficulty.level = DifficultyConfig::kNames[static_cast<usize>(code)];
    } else if (m_page == Page::Multiplayer) {
        next.multiplayer.mode = static_cast<MultiplayerMode>(code);
    } else if (m_page == Page::Compass) {
        next.camera.compass = code == 1;
    } else if (m_page == Page::Combat) {
        if (!flipSwitch(kCombatSwitches, code, next.combat)) {
            return;
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
        const auto result = m_controls.update(input, ticks, m_menu, m_config, m_persist,
                                              [this](s32 selected) { rebuild(selected); });
        if (result.closed) {
            m_page = Page::Root;
            rebuild();
            return {};
        }
        // A step back to the list of players is heard as the depth falls; a Back event
        // would close the settings.
        return result.event.action == MenuAction::Back ? MenuEvent{} : result.event;
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
    if ((m_page == Page::Combat || m_page == Page::Game) && !m_menu.closing() &&
        (input.left || input.right)) {
        change(input.left ? -1 : 1);
        return {MenuAction::Moved, 0};
    }
    if (m_page == Page::Graphics && !m_menu.closing() && (input.left || input.right)) {
        const auto selected = m_menu.selection();
        if (selected >= kVideoRows) {
            rebuild(kVideoRows + (selected - kVideoRows + (input.left ? -1 : 1) + 2) % 2);
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
            // Restore Defaults stages the shipped video settings for Apply.
            const GameConfig defaults;
            const VideoView view{m_scope, m_display, resolutions(), shownMonitor()};
            for (const VideoRow& row : kVideoTable) {
                restoreRow(row, m_config, defaults, view);
            }
            rebuild(kVideoRows + 1);
        } else if (event.direction != 0) {
            change(event.direction);
        }
        return {};
    }
    if (event.action == MenuAction::Back) {
        if (m_page == Page::Graphics) {
            m_config = m_video.saved();
        }
        if (m_page == Page::Root) {
            return {MenuAction::Back, 0};
        }
        // Back from one of the Game page's own pages lands on the row that opened it.
        if (const PageLink* link = linkTo(kGameLinks, m_page)) {
            m_page = Page::Game;
            m_notice.clear();
            rebuild(link->code);
            return {};
        }
        const PageLink* link = linkTo(kRootLinks, m_page);
        const s32 previousCode = link != nullptr ? link->code : 0;
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
            const PageLink* link = linkOf(kRootLinks, event.code);
            if (link == nullptr) {
                return event;
            }
            if (link->page == Page::Controls) {
                m_page = Page::Controls;
                m_controls.begin(m_config);
                rebuild();
                return event;
            }
            m_page = link->page;
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
            rebuild(currentChoice());
        } else if (const PageLink* link =
                       m_page == Page::Game ? linkOf(kGameLinks, event.code) : nullptr) {
            m_page = link->page;
            rebuild(currentChoice());
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
s32 SettingsMenu::currentChoice() const {
    switch (m_page) {
    case Page::Difficulty:
        for (usize i = 0; i < DifficultyConfig::kNames.size(); ++i) {
            if (DifficultyConfig::kNames[i] == m_config.difficulty.level) {
                return static_cast<s32>(i);
            }
        }
        return 1; // an unknown level opens on the normal one
    case Page::Multiplayer: return static_cast<s32>(m_config.multiplayer.mode);
    case Page::Compass: return m_config.camera.compass ? 1 : 0;
    default: return 0;
    }
}
s32 SettingsMenu::depth() const {
    if (m_page == Page::Root) {
        return 0;
    }
    return m_page == Page::Controls && m_controls.editing() ? 2 : 1;
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
    std::string notice = m_notice;
    if (notice.empty() && m_page == Page::Graphics && !m_confirmVideo && restartNeeded()) {
        notice = text("settings.rendererRestart");
    }
    if (!notice.empty()) {
        const bool low = m_page == Page::Audio || m_page == Page::Graphics;
        painter.draw(canvas, -m_screen.width / 2, low ? kLowNoticeY : kNoticeY, notice, style);
    }
}
} // namespace gdl::game
