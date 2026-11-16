#include "expansions/n64/hud/LegendsN64HudPainter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/assets/StringTable.h"
#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "expansions/n64/formats/LegendsN64Font.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/n64/hud/LegendsN64Frame.h"
#include "expansions/n64/hud/LegendsN64HudIcons.h"
#include "expansions/n64/hud/LegendsN64HudLayout.h"
#include "expansions/n64/hud/LegendsN64HudText.h"
#include "expansions/n64/hud/LegendsN64Scroll.h"
#include "expansions/shared/hud/LegendsHudArt.h"
#include "expansions/shared/hud/LegendsHudClock.h"
#include "expansions/shared/hud/LegendsHudStyleIds.h"
#include "game/characters/ClassData.h"
#include "game/hud/BossMeter.h"
#include "game/hud/ChallengeHud.h"
#include "game/hud/HelpMessages.h"
#include "game/hud/HudView.h"
#include "game/hud/PickupHud.h"
#include "game/hud/PowerupSelector.h"
#include "game/hud/StatusBox.h"
#include "game/party/PlayerRuntime.h"
#include "game/players/PickupVoices.h"

namespace gdl::game {
namespace {

using Layout = LegendsN64HudLayout;

constexpr std::string_view kColor = "COLOR";
constexpr std::string_view kEmpty = "COLORI";
constexpr std::string_view kBorder = "BORDER";
constexpr std::string_view kTurbo = "TURBO";
constexpr std::string_view kLights = "TURBOCLR";
constexpr std::string_view kHeart = "HEART1";
constexpr std::string_view kKey = "KEY1";
constexpr std::string_view kArrow = "ARROW";
/** Every picture of the HUD bank the panels draw. */
constexpr std::array<std::string_view, 12> kPanelPictures{
    kColor, kEmpty, kBorder,   kTurbo,    kLights,   kHeart,
    kKey,   kArrow, "POTIONB", "POTIONG", "POTIONR", "POTIONY"};
/** e00722b8: the HUD bank's advice scrolls, the plain one for a message at a place first, then
 * each costume colour's. */
constexpr std::array<std::string_view, 5> kHelpScrolls{
    "TEXTSCROLL", "YEL_TEXTSCROLL", "BLU_TEXTSCROLL", "RED_TEXTSCROLL", "GRE_TEXTSCROLL"};
constexpr std::string_view kGlass = "TIMER_P0";
constexpr std::string_view kSand = "TIMER_P1";

constexpr u8 kOpaque = 255;
constexpr f32 kMeterShares = 2; ///< a meter's two strips share its health between them

/** FUN_e0059b44's levels: a third unlit is 50, and one full 255. */
constexpr s32 kTurboUnlit = 50;
constexpr s32 kTurboLit = 255;
constexpr s32 kTurboRise = 100; ///< how far a third brightens as it fills
constexpr s32 kTurboFull = 100;
constexpr s32 kGreenFull = 33; ///< green fills to 33, then is lit whole from 34
constexpr s32 kYellowFull = 66;
constexpr s32 kRedSpan = 34; ///< red fills from 66 over 34, to 100

/** A shade of `shades` by costume colour; a colour past the four is held to them, as
 * FUN_e00477e0 holds the N64's. */
Color shadeOf(const std::array<Color, 4>& shades, s32 color) {
    return shades[static_cast<usize>(std::clamp(color, 0, static_cast<s32>(shades.size()) - 1))];
}

/** `color` at turbo level `level`, written opaque. */
Color litAt(Color color, s32 level) {
    const auto scaled = [level](u8 channel) {
        return static_cast<u8>((static_cast<s32>(channel) * level + kOpaque / 2) / kOpaque);
    };
    return Color::rgba(scaled(color.r), scaled(color.g), scaled(color.b));
}

/** Draws `texture` mirrored about its first texel to twice its width, and twice its height too
 * when `down`, from frame point (left, top), as a mirrored blit draws it. */
void drawMirrored(Canvas& canvas, const Texture& texture, f32 left, f32 top, bool down,
                  Color color) {
    const auto width = static_cast<f32>(texture.width());
    const auto height = static_cast<f32>(texture.height());
    const auto columns = legendsN64MirroredSpan(left, 2 * width, 0, width);
    const auto rows =
        down ? legendsN64MirroredSpan(top, 2 * height, 0, height)
             : std::array<std::optional<LegendsN64SpanPart>, 2>{
                   LegendsN64SpanPart{.start = top, .length = height, .from = 0, .to = 1},
                   std::nullopt};
    for (const auto& row : rows) {
        for (const auto& column : columns) {
            if (row && column) {
                canvas.draw(
                    texture, Rect{column->start, row->start, column->length, row->length},
                    Rect{column->from, row->from, column->to - column->from, row->to - row->from},
                    color);
            }
        }
    }
}

/** Draws `texture` whole with its centre at `centre` of the panel at (left, top), turned about
 * when `turned`. */
void drawCentred(Canvas& canvas, const Texture& texture, f32 left, f32 top, LegendsN64Point centre,
                 u8 opacity, bool turned = false) {
    const auto width = static_cast<f32>(texture.width());
    const auto height = static_cast<f32>(texture.height());
    const Rect area{left + static_cast<f32>(centre.x) - width / 2,
                    top + static_cast<f32>(centre.y) - height / 2, width, height};
    canvas.draw(texture, area, turned ? Rect{1, 0, -1, 1} : Rect{0, 0, 1, 1},
                Color::white().withAlpha(opacity));
}

/** Draws the part of `texture` within the portrait's clip, from its top left. */
void drawPortraitPart(Canvas& canvas, const Texture& texture, f32 left, f32 top, Color color) {
    const auto width = static_cast<f32>(std::min(texture.width(), u32{Layout::kPortraitClip.x}));
    const auto height = static_cast<f32>(std::min(texture.height(), u32{Layout::kPortraitClip.y}));
    canvas.draw(texture,
                Rect{left + static_cast<f32>(Layout::kPortrait.x),
                     top + static_cast<f32>(Layout::kPortrait.y), width, height},
                Rect{0, 0, width / static_cast<f32>(texture.width()),
                     height / static_cast<f32>(texture.height())},
                color);
}

/** The text `id` names, none without the table or the id in it. */
std::string_view words(const HudView& view, std::string_view id) {
    return view.strings != nullptr && view.strings->has(id) ? view.strings->get(id)
                                                            : std::string_view{};
}

/** The words a panel's class line shows: the N64's word for its class, else Dark Legacy's
 * lower-case name of it. */
std::string_view classWords(const HudView& view, const HudSeat& shown) {
    const bool pojo =
        shown.runtime != nullptr && PickupVoices::carriesPojo(shown.runtime->actor.save());
    const s32 classIndex = shown.box.classIndex;
    if (const auto native = legendsN64ClassText(classIndex, pojo)) {
        return words(view, legendsN64HudTextKey(*native));
    }
    // help.player_class_lc names the sixteen classes before Sumner, from one.
    if (classIndex >= 0 && classIndex < kSumnerClass) {
        return words(view, std::format("help.player_class_lc.{}", classIndex + 1));
    }
    return {};
}

/** A word cut to the ten characters a panel's "%.10s" fields show. */
std::string_view cut(std::string_view text) {
    return text.substr(0, std::min(text.size(), Layout::kWordLength));
}

/** The banked pictures of `file`, named `name` should they not be read. */
std::vector<formats::LegendsN64Texture> decodedBank(const std::filesystem::path& file,
                                                    std::string_view name) {
    try {
        return formats::decodeLegendsN64Textures(readFile(file));
    } catch (const std::exception& error) {
        throw FormatError(std::format("{}: {}", name, error.what()));
    }
}

/** The picture `name` of a decoded bank; null when the bank has none, or only a placeholder. */
formats::LegendsN64Texture* pictureIn(std::vector<formats::LegendsN64Texture>& bank,
                                      std::string_view name) {
    const auto found = std::ranges::find(bank, name, &formats::LegendsN64Texture::name);
    return found == bank.end() || found->external || found->image.pixels.empty() ? nullptr
                                                                                 : &*found;
}

/** Writes `text` with `font`'s painter from (x, y), in white. */
void write(Canvas& canvas, const TextPainter& font, s32 x, s32 y, std::string_view text) {
    if (font.ready() && !text.empty()) {
        font.draw(canvas, x, y, text, TextStyle{});
    }
}

} // namespace

std::array<s32, 3> legendsN64TurboLevels(f32 shown) {
    const s32 t =
        std::isfinite(shown)
            ? static_cast<s32>(std::clamp(std::floor(shown), 0.0f, static_cast<f32>(kTurboFull)))
            : 0;
    if (t >= kTurboFull) {
        return {kTurboLit, kTurboLit, kTurboLit};
    }
    if (t > kYellowFull) {
        return {kTurboLit, kTurboLit, kTurboUnlit + (t - kYellowFull) * kTurboRise / kRedSpan};
    }
    if (t > kGreenFull) {
        return {kTurboLit, kTurboUnlit + (t - kGreenFull) * kTurboRise / (kYellowFull - kGreenFull),
                kTurboUnlit};
    }
    return {kTurboUnlit + t * kTurboRise / kGreenFull, kTurboUnlit, kTurboUnlit};
}

std::array<std::optional<LegendsN64SpanPart>, 2> legendsN64MirroredSpan(f32 start, f32 length,
                                                                        f32 s, f32 size) {
    std::array<std::optional<LegendsN64SpanPart>, 2> parts;
    const f32 end = s + length;
    if (s < size) {
        const f32 stop = std::min(end, size);
        parts[0] = LegendsN64SpanPart{.start = start,
                                      .length = stop - s,
                                      .from = (size - s) / size,
                                      .to = (size - stop) / size};
    }
    if (end > size) {
        const f32 first = std::max(s, size);
        parts[1] = LegendsN64SpanPart{.start = start + (first - s),
                                      .length = end - first,
                                      .from = (first - size) / size,
                                      .to = (end - size) / size};
    }
    return parts;
}

LegendsN64HelpBox legendsN64HelpBox(s32 widest, usize lines, s32 lineHeight, Vec2 centre) {
    // FUN_e00717a0 tries each width in turn until its wrap holds the text's widest line in no
    // more than three lines; Dark Legacy's lines stand as they are, whatever the width.
    usize choice = Layout::kHelpWidths.size() - 1;
    if (lines <= Layout::kHelpLinesMost) {
        const auto holds =
            std::ranges::find_if(Layout::kHelpWraps, [widest](s32 wrap) { return widest < wrap; });
        if (holds != Layout::kHelpWraps.end()) {
            choice = static_cast<usize>(holds - Layout::kHelpWraps.begin());
        }
    }
    const s32 width = Layout::kHelpWidths.at(choice);
    const s32 height = static_cast<s32>(lines) * lineHeight + Layout::kHelpMargin;
    const auto whole = [](f32 value) {
        return std::isfinite(value) ? static_cast<s32>(std::lround(value)) : 0;
    };
    const s32 x = std::clamp(whole(centre.x - static_cast<f32>(width) / 2), 0,
                             std::max(static_cast<s32>(LegendsN64Frame::kWidth) - width, 0));
    const s32 y = std::clamp(whole(centre.y - static_cast<f32>(height) / 2), Layout::kHelpTop,
                             std::max(Layout::kStripTop - height, Layout::kHelpTop));
    // FUN_e0071a10 sets the text in by width/70 * 12, rounded.
    const s32 inset =
        (width * Layout::kHelpInset + Layout::kHelpInsetSpan / 2) / Layout::kHelpInsetSpan;
    return {.x = x,
            .y = y,
            .width = width,
            .height = height,
            .textX = x + inset,
            .textY = y + Layout::kHelpTextTop};
}

std::array<s32, 2> legendsN64MeterWidths(f32 share, f32 strip, f32 leftInset, f32 rightInset) {
    const f32 f = std::isfinite(share) ? std::clamp(share, 0.0f, kMeterShares) : 0.0f;
    return {f >= 1.0f ? static_cast<s32>(strip)
                      : static_cast<s32>(leftInset + f * (strip - leftInset)),
            f > 1.0f ? static_cast<s32>((f - 1.0f) * (strip - rightInset)) : 0};
}

s32 legendsN64MeterStep(f32 maxHealth) {
    if (!std::isfinite(maxHealth) || maxHealth <= 0) {
        return 0;
    }
    return static_cast<s32>(std::floor(maxHealth / static_cast<f32>(Layout::kMeterSteps)));
}

bool LegendsN64HudPainter::load(RenderDevice& device, const std::filesystem::path& root,
                                DarkLegacyHudArt& darkLegacy, std::span<const HudMember> party) {
    release();
    const AssetLocator files{root};
    const auto bank = files.find(kHudBank);
    const auto smallFont = files.find(kSmallFontFile);
    const auto nameFont = files.find(kNameFontFile);
    const auto numberFont = files.find(kNumberFontFile);
    if (!bank || !smallFont || !nameFont || !numberFont) {
        return false;
    }
    try {
        loadPictures(device, *bank);
        loadFont(device, *smallFont, kSmallFontFile, m_small);
        loadFont(device, *nameFont, kNameFontFile, m_name);
        loadFont(device, *numberFont, kNumberFontFile, m_numbers);
    } catch (...) {
        release();
        throw;
    }
    loadTimer(device, files);
    std::array<bool, kClassCount> taken{};
    for (const HudMember& member : party) {
        if (member.classIndex < 0 || member.classIndex >= kClassCount) {
            continue;
        }
        if (bool& done = taken.at(static_cast<usize>(member.classIndex)); !done) {
            done = true;
            loadPortrait(device, files, member.classIndex);
        }
    }
    borrowCountIcons(darkLegacy);
    return true;
}

void LegendsN64HudPainter::release() {
    for (Font* font : {&m_small, &m_name, &m_numbers}) {
        font->text.setFont(nullptr, nullptr);
        font->sheet.reset();
        font->glyphs = {};
    }
    m_portraits = {};
    m_color = m_empty = m_border = m_turbo = m_lights = m_heart = m_key = m_arrow = nullptr;
    m_glass = m_sand = nullptr;
    m_grains = {};
    m_countIcons.clear();
    m_clock = {};
    m_meters = {};
    m_art.clear();
}

void LegendsN64HudPainter::loadPictures(RenderDevice& device, const std::filesystem::path& bank) {
    auto pictures = decodedBank(bank, kHudBank);
    const auto take = [&](std::string_view name) {
        formats::LegendsN64Texture* picture = pictureIn(pictures, name);
        if (picture == nullptr) {
            throw FormatError(std::format("{} holds no {}", kHudBank, name));
        }
        std::vector<u8>& pixels = picture->image.pixels;
        if (name == kColor || name == kEmpty) {
            // I8 decodes each texel's intensity as its alpha too; the N64 draws these
            // backgrounds opaque (FUN_e0057f50's combiner).
            for (usize alpha = 3; alpha < pixels.size(); alpha += 4) {
                pixels[alpha] = kOpaque;
            }
        }
        m_art.add(std::string{name},
                  uploadHudTexture(device, picture->image.width, picture->image.height, pixels));
    };
    for (const std::string_view name : kPanelPictures) {
        take(name);
    }
    for (const std::string_view name : kHelpScrolls) {
        take(name);
    }
    m_color = m_art.find(kColor);
    m_empty = m_art.find(kEmpty);
    m_border = m_art.find(kBorder);
    m_turbo = m_art.find(kTurbo);
    m_lights = m_art.find(kLights);
    m_heart = m_art.find(kHeart);
    m_key = m_art.find(kKey);
    m_arrow = m_art.find(kArrow);
}

void LegendsN64HudPainter::loadFont(RenderDevice& device, const std::filesystem::path& file,
                                    std::string_view name, Font& font) {
    formats::LegendsN64Font decoded;
    try {
        decoded = formats::LegendsN64Font::parse(readFile(file));
    } catch (const std::exception& error) {
        throw FormatError(std::format("{}: {}", name, error.what()));
    }
    font.glyphs = std::move(decoded.font);
    font.sheet =
        uploadHudTexture(device, decoded.atlas.width, decoded.atlas.height, decoded.atlas.pixels);
    font.text.setFont(&font.glyphs, font.sheet.get());
}

void LegendsN64HudPainter::loadPortrait(RenderDevice& device, const AssetLocator& files,
                                        s32 classIndex) {
    const auto bank = legendsN64ClassBank(classIndex);
    const auto code = legendsN64ClassCode(classIndex);
    if (!bank || !code) {
        return;
    }
    const std::string name = std::format("{}/objects.wad", *bank);
    const auto file = files.find(name);
    if (!file) {
        return;
    }
    Portrait& portrait = m_portraits.at(static_cast<usize>(classIndex));
    try {
        auto pictures = formats::decodeLegendsN64Textures(readFile(*file));
        const auto take = [&](const std::string& picture) -> const Texture* {
            const formats::LegendsN64Texture* found = pictureIn(pictures, picture);
            if (found == nullptr) {
                return nullptr;
            }
            m_art.add(picture, uploadHudTexture(device, found->image.width, found->image.height,
                                                found->image.pixels));
            return m_art.find(picture);
        };
        portrait.light = take(std::format("{}_P1", *code));
        portrait.shine = take(std::format("{}_P2", *code));
    } catch (const std::exception& error) {
        log::warn("N64 HUD: {}: {}; the {} portrait is left out", name, error.what(), *code);
    }
}

void LegendsN64HudPainter::loadTimer(RenderDevice& device, const AssetLocator& files) {
    const auto bank = files.find(kTimerBank);
    if (!bank) {
        return;
    }
    // The grains' frames, then the glass and its sand.
    std::vector<std::string> names;
    for (usize frame = 0; frame < Layout::kSandFrames; ++frame) {
        names.push_back(std::format("SAND_ANIM{:02}", frame));
    }
    names.emplace_back(kGlass);
    names.emplace_back(kSand);
    try {
        auto pictures = formats::decodeLegendsN64Textures(readFile(*bank));
        // The hourglass is drawn whole or not at all: every picture is found before any is
        // uploaded.
        std::vector<const formats::LegendsN64Texture*> found;
        for (const std::string& name : names) {
            const formats::LegendsN64Texture* picture = pictureIn(pictures, name);
            if (picture == nullptr) {
                throw FormatError(std::format("no {}", name));
            }
            found.push_back(picture);
        }
        for (usize index = 0; index < names.size(); ++index) {
            const formats::LegendsN64Texture& picture = *found.at(index);
            m_art.add(names.at(index),
                      uploadHudTexture(device, picture.image.width, picture.image.height,
                                       picture.image.pixels));
        }
        for (usize frame = 0; frame < m_grains.size(); ++frame) {
            m_grains.at(frame) = m_art.find(names.at(frame));
        }
        m_glass = m_art.find(kGlass);
        m_sand = m_art.find(kSand);
    } catch (const std::exception& error) {
        m_glass = m_sand = nullptr;
        m_grains = {};
        log::warn("N64 HUD: {}: {}; the hourglass is Dark Legacy's", kTimerBank, error.what());
    }
}

void LegendsN64HudPainter::borrowCountIcons(DarkLegacyHudArt& darkLegacy) {
    // Every icon a count may show is taken now, so that drawing looks nothing up.
    for (std::string& icon : PickupHud::countIcons()) {
        if (const Texture* picture = darkLegacy.countArt(icon)) {
            m_countIcons.emplace(std::move(icon), picture);
        }
    }
}

f32 LegendsN64HudPainter::statusTop() const {
    return LegendsN64Frame::toCanvas({0.0f, static_cast<f32>(Layout::kStripTop)}).y;
}

void LegendsN64HudPainter::prepare(HudFrame& frame, const HudView& view) {
    // The parts drawn in Dark Legacy's look are prepared as that look prepares them.
    frame.darkLegacy.look().prepare(frame, view);
    const s64 steps = m_clock.advance(view.seconds);
    easeMeters(view, steps, m_clock.rewound());
}

void LegendsN64HudPainter::easeMeters(const HudView& view, s64 steps, bool rewound) {
    const usize bound = std::min(view.bossCount, m_meters.size());
    // FUN_e08887b8 sets the one step every meter takes as each is bound: the last sets it.
    const s32 step =
        bound == 0 ? 0 : legendsN64MeterStep(view.bossReadings.at(bound - 1).maxHealth);
    for (usize index = 0; index < m_meters.size(); ++index) {
        ShownMeter& shown = m_meters.at(index);
        if (index >= bound) {
            shown = {};
            continue;
        }
        const HudBossReading& reading = view.bossReadings.at(index);
        const f32 target = std::max(reading.health, 0.0f);
        // A meter first seen or bound anew shows its boss's health as it is, and so does every
        // meter after a rewind or while a step would move it nowhere.
        if (!shown.seen || shown.maxHealth != reading.maxHealth || rewound || step <= 0) {
            shown = {.health = target, .maxHealth = reading.maxHealth, .seen = true};
            continue;
        }
        const f32 move = static_cast<f32>(step) * static_cast<f32>(steps);
        shown.health = shown.health > target ? std::max(shown.health - move, target)
                                             : std::min(shown.health + move, target);
    }
}

void LegendsN64HudPainter::drawStatus(HudFrame& frame, const HudView& view) {
    static_assert(HudView::kSeats == Layout::kSlotX.size());
    {
        const CanvasFrame native(frame.canvas, LegendsN64Frame::transform());
        // FUN_e005835c paints every panel's background, frame and pictures, then the words over
        // them, then the turbo meters' lights.
        for (usize seat = 0; seat < view.seats.size(); ++seat) {
            drawPanel(frame.canvas, seat, view.seats.at(seat));
        }
        for (usize seat = 0; seat < view.seats.size(); ++seat) {
            drawWords(frame.canvas, seat, view.seats.at(seat), view);
        }
        for (usize seat = 0; seat < view.seats.size(); ++seat) {
            drawLights(frame.canvas, seat, view.seats.at(seat));
        }
    }
    drawCounts(frame.canvas, view);
}

void LegendsN64HudPainter::drawRuneMeter(HudFrame& frame, const HudView& view) {
    // The N64 has no runestone finder.
    frame.darkLegacy.look().drawRuneMeter(frame, view);
}

void LegendsN64HudPainter::drawHourglass(HudFrame& frame, const HudView& view) {
    if (m_glass == nullptr) {
        frame.darkLegacy.look().drawHourglass(frame, view);
        return;
    }
    if (!view.hourglass || !std::isfinite(view.hourglass->elapsed)) {
        return;
    }
    const f32 gone = std::clamp(view.hourglass->elapsed, 0.0f, 1.0f);
    const bool running = view.hourglass->fallingFrame >= 0;
    const CanvasFrame native(frame.canvas, LegendsN64Frame::transform());
    const Color clouded = Color::white().withAlpha(kOpaque - Layout::kTimerTransparency);
    const auto left = static_cast<f32>(Layout::kTimer.x);
    const auto top = static_cast<f32>(Layout::kTimer.y);
    frame.canvas.draw(
        *m_glass,
        Rect{left, top, static_cast<f32>(m_glass->width()), static_cast<f32>(m_glass->height())},
        clouded);
    // TIMER_P1's rows [from, to), where the glass holds them.
    const auto sand = [&](f32 from, f32 to) {
        if (to <= from) {
            return;
        }
        const auto rows = static_cast<f32>(m_sand->height());
        frame.canvas.draw(*m_sand,
                          Rect{left, top + from, static_cast<f32>(m_sand->width()), to - from},
                          Rect{0, from / rows, 1, (to - from) / rows}, clouded);
    };
    const f32 fallen = static_cast<f32>(Layout::kSandRows) * gone;
    const auto upper = static_cast<f32>(Layout::kUpperSandRow);
    const auto lower = static_cast<f32>(Layout::kLowerSandRow);
    sand(upper + fallen, upper + static_cast<f32>(Layout::kSandRows));
    // While the clock stands still the bottom bulb and the grains are hidden.
    if (!running) {
        return;
    }
    sand(lower - fallen, lower);
    const Texture* grains =
        m_grains.at(static_cast<usize>(view.hourglass->fallingFrame) % m_grains.size());
    const f32 stream = static_cast<f32>(Layout::kSandRows) - fallen;
    if (grains != nullptr && stream > 0) {
        frame.canvas.draw(
            *grains,
            Rect{static_cast<f32>(Layout::kSandStream.x), static_cast<f32>(Layout::kSandStream.y),
                 static_cast<f32>(grains->width()), stream},
            Rect{0, 0, 1, stream / static_cast<f32>(grains->height())}, Color::white());
    }
}

void LegendsN64HudPainter::drawBossMeters(HudFrame& frame, const HudView& view) {
    if (view.bossMeters == nullptr) {
        return;
    }
    // An N64 boss's art stands in the frame; art drawn for the canvas, a Dark Legacy boss's,
    // stands in the canvas at the row the frame's first lands on.
    const bool native = view.levelHud == kLegendsN64HudStyle;
    std::optional<CanvasFrame> inFrame;
    if (native) {
        inFrame.emplace(frame.canvas, LegendsN64Frame::transform());
    }
    const auto strip = static_cast<f32>(native ? Layout::kMeterStripWidth : BossMeter::kPieceWidth);
    const auto top = static_cast<f32>(native ? Layout::kMeterTop : Layout::kForeignMeterTop);
    const f32 insetScale = strip / static_cast<f32>(BossMeter::kPieceWidth);
    const BossMeters& meters = *view.bossMeters;
    for (usize index = 0; index < meters.count(); ++index) {
        const BossMeter& meter = meters.meter(index);
        if (!meter.showing()) {
            continue;
        }
        const BossMeter::Layout layout = meter.layout();
        std::array<s32, BossMeter::kMostPieces> widths{};
        if (layout.pieces >= BossMeter::kMostPieces) {
            if (index >= view.bossCount || index >= m_meters.size()) {
                continue;
            }
            const HudBossReading& reading = view.bossReadings.at(index);
            const ShownMeter& shown = m_meters.at(index);
            const f32 health = shown.seen ? shown.health : std::max(reading.health, 0.0f);
            widths =
                legendsN64MeterWidths(kMeterShares * health / std::max(reading.maxHealth, 1.0f),
                                      strip, static_cast<f32>(layout.leftInset) * insetScale,
                                      static_cast<f32>(layout.rightInset) * insetScale);
        } else {
            // FUN_e0888a7c crops two strips alone, and no N64 boss has one: a single strip
            // keeps Dark Legacy's crop.
            widths = meter.look().widths;
        }
        const s32 pieces = std::min(layout.pieces, BossMeter::kMostPieces);
        for (s32 piece = 0; piece < pieces; ++piece) {
            const f32 x = static_cast<f32>(layout.left) + strip * static_cast<f32>(piece);
            if (const Texture* back =
                    meter.pieceTexture(frame.device, BossMeter::MeterPart::Background, piece)) {
                frame.canvas.draw(*back, Rect{x, top, strip, static_cast<f32>(back->height())});
            }
            const f32 width =
                std::clamp(static_cast<f32>(widths.at(static_cast<usize>(piece))), 0.0f, strip);
            if (width <= 0) {
                continue;
            }
            if (const Texture* fill =
                    meter.pieceTexture(frame.device, BossMeter::MeterPart::Fill, piece)) {
                // Cropped, not squeezed: the fill's left part.
                frame.canvas.draw(*fill, Rect{x, top, width, static_cast<f32>(fill->height())},
                                  Rect{0, 0, width / strip, 1}, Color::white());
            }
        }
    }
}

void LegendsN64HudPainter::drawSelectors(HudFrame& frame, const HudView& view) {
    frame.darkLegacy.look().drawSelectors(frame, view);
}

void LegendsN64HudPainter::drawHelp(HudFrame& frame, const HudView& view) {
    if (!view.help || view.help->lines.empty() || !m_name.text.ready()) {
        return;
    }
    const HudHelp& help = *view.help;
    s32 widest = 0;
    for (const std::string& line : help.lines) {
        widest = std::max(widest, m_name.text.measure(line));
    }
    // Dark Legacy's place for it, over its player's head or else the screen's middle, and its
    // lines and timing; the N64's scroll, size and ink. It stands still: the N64's own advice
    // fades in and holds the game, which Dark Legacy's help does not.
    const Vec2 centre = LegendsN64Frame::toFrame(
        {help.head.x, help.head.y - static_cast<f32>(HelpMessages::kAboveHead)});
    const s32 lineHeight = m_name.text.lineHeight();
    const LegendsN64HelpBox box = legendsN64HelpBox(widest, help.lines.size(), lineHeight, centre);
    const CanvasFrame native(frame.canvas, LegendsN64Frame::transform());
    if (const Texture* scroll = helpScroll(help.color)) {
        drawLegendsN64Scroll(frame.canvas, *scroll,
                             Rect{static_cast<f32>(box.x), static_cast<f32>(box.y),
                                  static_cast<f32>(box.width), static_cast<f32>(box.height)});
    }
    const TextStyle ink{.color = Color::black()};
    s32 y = box.textY;
    for (const std::string& line : help.lines) {
        m_name.text.draw(frame.canvas, box.textX, y, line, ink);
        y += lineHeight;
    }
}

const Texture* LegendsN64HudPainter::helpScroll(s32 color) const {
    const bool coloured = color >= 0 && static_cast<usize>(color) + 1 < kHelpScrolls.size();
    return m_art.find(kHelpScrolls.at(coloured ? static_cast<usize>(color) + 1 : 0));
}

void LegendsN64HudPainter::drawPanel(Canvas& canvas, usize seat, const HudSeat& shown) const {
    const auto left = static_cast<f32>(Layout::kSlotX.at(seat));
    const auto top = static_cast<f32>(Layout::kStripTop);
    const StatusBoxView& box = shown.box;
    // A seat nobody plays is empty in its own colour (FUN_e0059144); a player fallen to the
    // tower leaves the empty panel in the costume's.
    if (box.mode == StatusBoxView::Mode::Plain) {
        drawEmpty(canvas, left, shadeOf(Layout::kDark, static_cast<s32>(seat)));
        return;
    }
    if (box.inTower) {
        drawEmpty(canvas, left, shadeOf(Layout::kDark, box.color));
        return;
    }
    const Color bright = shadeOf(Layout::kBright, box.color);
    if (m_color != nullptr) {
        drawMirrored(canvas, *m_color, left, top, true, bright);
    }
    if (box.classIndex >= 0 && box.classIndex < kClassCount) {
        const Portrait& portrait = m_portraits.at(static_cast<usize>(box.classIndex));
        if (portrait.light != nullptr) {
            drawPortraitPart(canvas, *portrait.light, left, top, bright);
        }
        if (portrait.shine != nullptr) {
            drawPortraitPart(canvas, *portrait.shine, left, top,
                             Color::white().withAlpha(Layout::kShineAlpha));
        }
    }
    if (m_border != nullptr) {
        drawMirrored(canvas, *m_border, left, top, false, Color::white());
    }
    if (m_turbo != nullptr) {
        drawMirrored(canvas, *m_turbo, left, top, false, Color::white());
    }
    // FUN_e0056acc names the key and the bottle only while some are carried. Dark Legacy's
    // health may stand past its level's most, which is full health still.
    if (box.keys > 0 && m_key != nullptr) {
        drawCentred(canvas, *m_key, left, top, Layout::kKey, kOpaque);
    }
    if (box.potions > 0) {
        if (const Texture* bottle = m_art.find(legendsN64PotionBottle(box.potionKind))) {
            drawCentred(canvas, *bottle, left, top, Layout::kPotion, kOpaque);
        }
    }
    if (m_heart != nullptr) {
        const bool full = shown.mostHealth > 0 && box.health >= shown.mostHealth;
        drawCentred(canvas, *m_heart, left, top, Layout::kHeart,
                    full ? kOpaque : static_cast<u8>(kOpaque - Layout::kHeartShort));
    }
    if (m_arrow != nullptr) {
        const auto resting = static_cast<u8>(kOpaque - Layout::kArrowResting);
        drawCentred(canvas, *m_arrow, left, top, Layout::kLeftArrow, resting);
        drawCentred(canvas, *m_arrow, left, top, Layout::kRightArrow, resting, true);
    }
}

void LegendsN64HudPainter::drawEmpty(Canvas& canvas, f32 left, Color shade) const {
    const auto top = static_cast<f32>(Layout::kStripTop);
    if (m_empty != nullptr) {
        canvas.draw(*m_empty,
                    Rect{left, top, static_cast<f32>(m_empty->width()),
                         static_cast<f32>(m_empty->height())},
                    shade);
    }
    if (m_border != nullptr) {
        drawMirrored(canvas, *m_border, left, top, false, Color::white());
    }
}

void LegendsN64HudPainter::drawWords(Canvas& canvas, usize seat, const HudSeat& shown,
                                     const HudView& view) const {
    const s32 left = Layout::kSlotX.at(seat);
    const s32 top = Layout::kStripTop;
    const StatusBoxView& box = shown.box;
    if (box.mode == StatusBoxView::Mode::Plain) {
        return;
    }
    if (box.inTower) {
        // Dark Legacy's choices, centred in the empty panel; online none offers to quit.
        if (box.towerPrompt) {
            const s32 centre = -(left + Layout::kPromptCentre);
            write(canvas, m_small.text, centre, top + Layout::kPromptLineTops[0],
                  words(view, "hud.waitInTower"));
            if (box.quitOffered) {
                write(canvas, m_small.text, centre, top + Layout::kPromptLineTops[1],
                      words(view, "hud.quitGame"));
            }
        }
        return;
    }
    const s32 health = std::clamp(box.health, 0, Layout::kMostHealthShown);
    write(canvas, m_numbers.text, left + Layout::kHealth.x, top + Layout::kHealth.y,
          std::format("{:{}}", health, Layout::kHealthDigits));
    if (box.keys > 0) {
        write(canvas, m_small.text, left + Layout::kKeyCount.x, top + Layout::kKeyCount.y,
              std::format("{}", box.keys));
    }
    if (box.potions > 0) {
        write(canvas, m_small.text, left + Layout::kPotionCount.x, top + Layout::kPotionCount.y,
              std::format("{}", box.potions));
    }
    write(canvas, m_name.text, left + Layout::kName.x, top + Layout::kName.y, cut(box.name));
    write(canvas, m_small.text, left + Layout::kClass.x, top + Layout::kClass.y,
          cut(classWords(view, shown)));
}

void LegendsN64HudPainter::drawLights(Canvas& canvas, usize seat, const HudSeat& shown) const {
    if (!shown.turbo || m_lights == nullptr || shown.box.mode == StatusBoxView::Mode::Plain ||
        shown.box.inTower) {
        return;
    }
    const auto levels = legendsN64TurboLevels(*shown.turbo);
    const auto left = static_cast<f32>(Layout::kSlotX.at(seat));
    const auto top = static_cast<f32>(Layout::kStripTop);
    const auto width = static_cast<f32>(m_lights->width());
    const auto rows = static_cast<f32>(Layout::kTurboRows);
    const f32 down = rows / static_cast<f32>(m_lights->height());
    for (const LegendsN64TurboSpan& span : Layout::kTurboSpans) {
        const Color tint = litAt(Layout::kTurboColors.at(span.third), levels.at(span.third));
        const auto parts = legendsN64MirroredSpan(left + static_cast<f32>(span.left),
                                                  static_cast<f32>(span.right - span.left),
                                                  static_cast<f32>(span.s), width);
        for (const auto& part : parts) {
            if (part) {
                canvas.draw(*m_lights, Rect{part->start, top, part->length, rows},
                            Rect{part->from, 0, part->to - part->from, down}, tint);
            }
        }
    }
}

void LegendsN64HudPainter::drawCounts(Canvas& canvas, const HudView& view) const {
    if (view.pickups == nullptr || !m_small.text.ready()) {
        return;
    }
    // The N64 has no ticker: Dark Legacy's, centred over each panel in canvas units, its counts
    // written in font 2 at the frame's proportions wherever they stand.
    const auto icon = static_cast<f32>(StatusBoxPainter::kCountIconSize);
    const auto gap = static_cast<f32>(StatusBoxPainter::kCountIconGap);
    for (usize seat = 0; seat < view.seats.size(); ++seat) {
        const PowerupSelector* selector = view.seats.at(seat).selector;
        const s32 bottom = selector != nullptr && selector->showing()
                               ? PickupHud::countRowY(selector->labelY(StatusBoxPainter::kY))
                               : Layout::kTickerRestTop;
        const f32 middle =
            static_cast<f32>(Layout::kSlotX.at(seat)) + static_cast<f32>(Layout::kPanelWidth) / 2;
        const f32 centre = LegendsN64Frame::toCanvas({middle, 0.0f}).x;
        s32 line = 0;
        for (const PickupCount& held : view.pickups->counts(static_cast<s32>(seat))) {
            if (!held.showing()) {
                continue;
            }
            const std::string text = std::format("{}/{}", held.count, held.total);
            const f32 written =
                static_cast<f32>(m_small.text.measure(text)) * LegendsN64Frame::kColumn;
            const f32 left = centre - (icon + gap + written) / 2;
            const auto top = static_cast<f32>(bottom - line++ * (StatusBoxPainter::kCountIconSize +
                                                                 StatusBoxPainter::kCountRowGap));
            if (const auto found = m_countIcons.find(held.icon); found != m_countIcons.end()) {
                canvas.draw(*found->second, Rect{left, top, icon, icon});
            }
            const CanvasFrame pen(
                canvas,
                glm::scale(glm::translate(
                               Mat4{1.0f},
                               Vec3{left + icon + gap,
                                    top + static_cast<f32>(StatusBoxPainter::kCountTextDy), 0.0f}),
                           Vec3{LegendsN64Frame::kColumn, LegendsN64Frame::kRow, 1.0f}));
            write(canvas, m_small.text, 0, 0, text);
        }
    }
}

} // namespace gdl::game
