#include "expansions/dreamcast/hud/LegendsDreamcastHudPainter.h"

#include <algorithm>
#include <array>
#include <exception>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/assets/BitmapFont.h"
#include "engine/assets/StringTable.h"
#include "engine/core/Error.h"
#include "engine/core/Log.h"
#include "engine/core/Strings.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"
#include "engine/math/Math.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "expansions/dreamcast/formats/LegendsDreamcastTexture.h"
#include "expansions/dreamcast/hud/LegendsDreamcastHudLayout.h"
#include "expansions/shared/formats/LegendsMap.h"
#include "expansions/shared/hud/LegendsHudArt.h"
#include "formats/FontFile.h"
#include "game/characters/ClassData.h"
#include "game/hud/HudView.h"
#include "game/hud/StatusBox.h"
#include "game/party/PlayerRuntime.h"

namespace gdl::game {
namespace {

using Layout = LegendsDreamcastHudLayout;

constexpr std::string_view kStrip = "BK_RUNE_STONE_02";
constexpr std::string_view kLevelLabel = "BTMBK_LEVL";
constexpr std::string_view kGoldLabel = "BTMBK_GOLD";
constexpr std::string_view kHealthLabel = "BTMBK_HELTH";
constexpr std::string_view kKeyIcon = "KEY_ICON";
constexpr std::string_view kPanelPrefix = "BTMBK_";    ///< a costume's panel, by colour
constexpr std::string_view kPlainStripPrefix = "S3_W"; ///< a kind-0 box's strip, by colour
constexpr std::string_view kPlainPanelPrefix = "S4_W"; ///< and its panel
constexpr std::string_view kBannerSuffix = "_NAME";    ///< a class's banner, after its code
constexpr std::string_view kCostumeObjects = "objects.rom";
constexpr std::string_view kCostumeTextures = "textures.rom";
/** The fonts' sheets in the static set: font handles 1, 4 and 6. */
constexpr std::string_view kSmallSheet = "8HI_FONTS5";
constexpr std::string_view kScoreSheet = "SCORE";
constexpr std::string_view kLargeSheet = "FONT32";
/** 8c14864c: the space widths of font handles 1, 4 and 6. */
constexpr s32 kSmallSpace = 8;
constexpr s32 kScoreSpace = 9;
constexpr s32 kLargeSpace = 16;
/** A native font: a header, then glyph records up to an empty one. */
constexpr usize kFontHeaderBytes = 12;
constexpr usize kGlyphRecordBytes = 16;
/** The class whose panels stand in for a class's own: every class's panel of a colour is the
 * same picture. */
constexpr std::string_view kStandInClass = "WAR";
/** The N64 HUD's word for Sumner's class (english/game.wad string 138); the Dreamcast names him
 * only in "Sumner's Tower" (1ST_READ.BIN 8c11af74). */
constexpr std::string_view kSumnerWord = "legends.n64.hud.text.138";
constexpr std::string_view kInTowerWord = "hud.inTower";
constexpr u8 kOpaque = 255;

/** The picture `name` of a decoded bank; null when the bank has none, or only a borrowed slot. */
const formats::LegendsMapTexture* pictureIn(std::span<const formats::LegendsMapTexture> bank,
                                            std::string_view name) {
    const auto found = std::ranges::find(bank, name, &formats::LegendsMapTexture::name);
    return found == bank.end() || found->external || found->image.pixels.empty() ? nullptr
                                                                                 : &*found;
}

std::unique_ptr<Texture> upload(RenderDevice& device, const formats::LegendsMapTexture& picture) {
    return uploadHudTexture(device, picture.image.width, picture.image.height,
                            picture.image.pixels);
}

/** The costume bank of class `code` in colour folder `colour`, players/<code>/<colour>. */
std::string costumeFolder(std::string_view code, std::string_view colour) {
    return std::format("players/{}/{}", toLowerAscii(code), colour);
}

/** The decoded pictures of the costume bank in `folder`; none when it is not there, and none,
 * with a warning, when it cannot be read. */
std::optional<std::vector<formats::LegendsMapTexture>> costumeBank(const AssetLocator& files,
                                                                   const std::string& folder) {
    const auto objects = files.find(std::format("{}/{}", folder, kCostumeObjects));
    const auto textures = files.find(std::format("{}/{}", folder, kCostumeTextures));
    if (!objects || !textures) {
        return std::nullopt;
    }
    try {
        return formats::decodeLegendsDreamcastTextures(readFile(*objects), readFile(*textures));
    } catch (const std::exception& error) {
        log::warn("Dreamcast HUD: {}: {}; its pictures are left out", folder, error.what());
        return std::nullopt;
    }
}

/** The ink of costume colour `color`; white past the four. */
Color inkOf(s32 color) {
    return color >= 0 && static_cast<usize>(color) < Layout::kTextColours.size()
               ? Layout::kTextColours.at(static_cast<usize>(color))
               : Color::white();
}

/** The picture of `pictures` for costume colour `color`; null past the four. */
const Texture* colourPicture(const std::array<const Texture*, kColorCount>& pictures, s32 color) {
    return color >= 0 && color < kColorCount ? pictures.at(static_cast<usize>(color)) : nullptr;
}

/** The text `id` names, none without the table or the id in it. */
std::string_view words(const HudView& view, std::string_view id) {
    return view.strings != nullptr && view.strings->has(id) ? view.strings->get(id)
                                                            : std::string_view{};
}

/** The words that name class `classIndex` where it has no banner: Dark Legacy's lower-case name
 * of it, and Sumner the N64 HUD's word, as Dark Legacy's names stop before him. */
std::string_view classWords(const HudView& view, s32 classIndex) {
    if (classIndex == kSumnerClass) {
        return words(view, kSumnerWord);
    }
    // help.player_class_lc names the sixteen classes before Sumner, from one.
    if (classIndex >= 0 && classIndex < kSumnerClass) {
        return words(view, std::format("help.player_class_lc.{}", classIndex + 1));
    }
    return {};
}

/** Draws `texture` at its own size from (left, top). */
void drawWhole(Canvas& canvas, const Texture* texture, f32 left, f32 top) {
    if (texture != nullptr) {
        canvas.draw(*texture, Rect{left, top, static_cast<f32>(texture->width()),
                                   static_cast<f32>(texture->height())});
    }
}

/** Draws the rows of panel `texture` the screen shows, from `left` at the panels' top. */
void drawPanelArt(Canvas& canvas, const Texture* texture, f32 left) {
    if (texture == nullptr || texture->height() == 0) {
        return;
    }
    const auto height = static_cast<f32>(texture->height());
    const f32 rows = std::min(static_cast<f32>(Layout::kPanelRows), height);
    canvas.draw(
        *texture,
        Rect{left, static_cast<f32>(Layout::kPanelTop), static_cast<f32>(texture->width()), rows},
        Rect{0, 0, 1, rows / height}, Color::white());
}

/** Writes `text` with `font` from (x, y), a negative x centring it on -x. */
void write(Canvas& canvas, const TextPainter& font, s32 x, s32 y, std::string_view text,
           const TextStyle& style) {
    if (font.ready() && !text.empty()) {
        font.draw(canvas, x, y, text, style);
    }
}

/** Whether seat `shown`'s player stands, its turbo meter showing: not dying. */
bool standing(const HudSeat& shown) {
    return shown.turbo.has_value();
}

} // namespace

bool LegendsDreamcastHudPainter::load(RenderDevice& device, const std::filesystem::path& root,
                                      DarkLegacyHudArt& /*darkLegacy*/,
                                      std::span<const HudMember> party) {
    release();
    const AssetLocator files{root};
    const auto objects = files.find(kStaticObjects);
    const auto textures = files.find(kStaticTextures);
    const auto smallFont = files.find(kSmallFontFile);
    const auto scoreFont = files.find(kScoreFontFile);
    const auto largeFont = files.find(kLargeFontFile);
    if (!objects || !textures || !smallFont || !scoreFont || !largeFont) {
        return false;
    }
    try {
        loadStatic(device, *objects, *textures);
        loadFont(*smallFont, kSmallFontFile, kSmallSpace, kSmallSheet, m_small);
        loadFont(*scoreFont, kScoreFontFile, kScoreSpace, kScoreSheet, m_score);
        loadFont(*largeFont, kLargeFontFile, kLargeSpace, kLargeSheet, m_large);
    } catch (...) {
        release();
        throw;
    }
    for (const HudMember& member : party) {
        loadClass(device, files, member.classIndex, member.color);
    }
    return true;
}

void LegendsDreamcastHudPainter::release() {
    for (Font* font : {&m_small, &m_score, &m_large}) {
        font->text.setFont(nullptr, nullptr);
        font->glyphs = {};
    }
    m_strip = m_levelLabel = m_goldLabel = m_healthLabel = m_key = m_glow = nullptr;
    m_plainStrips = {};
    m_plainPanels = {};
    m_classes = {};
    m_clock = {};
    m_glowSteps = {};
    m_art.clear();
}

void LegendsDreamcastHudPainter::loadStatic(RenderDevice& device,
                                            const std::filesystem::path& objects,
                                            const std::filesystem::path& textures) {
    std::vector<formats::LegendsMapTexture> pictures;
    try {
        pictures = formats::decodeLegendsDreamcastTextures(readFile(objects), readFile(textures));
    } catch (const std::exception& error) {
        throw FormatError(
            std::format("{} with {}: {}", kStaticObjects, kStaticTextures, error.what()));
    }
    // Every picture the boxes draw is taken whole before the look is drawn; nothing else of the
    // set is uploaded.
    const auto take = [&](std::string_view name) {
        const formats::LegendsMapTexture* picture = pictureIn(pictures, name);
        if (picture == nullptr) {
            throw FormatError(std::format("{} holds no {}", kStaticObjects, name));
        }
        m_art.add(std::string{name}, upload(device, *picture));
        return m_art.find(name);
    };
    m_strip = take(kStrip);
    m_levelLabel = take(kLevelLabel);
    m_goldLabel = take(kGoldLabel);
    m_healthLabel = take(kHealthLabel);
    m_key = take(kKeyIcon);
    for (usize color = 0; color < Layout::kPanelColours.size(); ++color) {
        const std::string_view colour = Layout::kPanelColours.at(color);
        m_plainStrips.at(color) = take(std::format("{}{}", kPlainStripPrefix, colour));
        m_plainPanels.at(color) = take(std::format("{}{}", kPlainPanelPrefix, colour));
    }
    // Dark Legacy's potion icons are the Dreamcast's, by the same kinds (8c14bd20).
    for (const std::string_view icon : StatusBoxPainter::kPotionIcons) {
        if (m_art.find(icon) == nullptr) {
            take(icon);
        }
    }
    for (const LegendsDreamcastTurboZone& zone : Layout::kTurboZones) {
        take(zone.art);
    }
    take(Layout::kTurboFullArt);
    m_glow = take(Layout::kTurboGlowArt);
    for (const std::string_view sheet : {kSmallSheet, kScoreSheet, kLargeSheet}) {
        take(sheet);
    }
}

void LegendsDreamcastHudPainter::loadFont(const std::filesystem::path& file, std::string_view name,
                                          s32 space, std::string_view sheet, Font& font) {
    formats::FontFile parsed;
    try {
        const std::vector<u8> bytes = readFile(file);
        parsed = formats::FontFile::parse(bytes);
        // The glyph table ends at an empty record: a file without one was cut short.
        if (parsed.glyphs.size() >= (bytes.size() - kFontHeaderBytes) / kGlyphRecordBytes) {
            throw FormatError("its glyph table has no end");
        }
    } catch (const std::exception& error) {
        throw FormatError(std::format("{}: {}", name, error.what()));
    }
    std::vector<BitmapGlyph> glyphs;
    glyphs.reserve(parsed.glyphs.size());
    for (const formats::FontGlyph& glyph : parsed.glyphs) {
        glyphs.push_back({.code = glyph.code, .width = glyph.width, .x = glyph.x, .y = glyph.y});
    }
    font.glyphs = BitmapFont::fromGlyphs(parsed.height, space, std::move(glyphs));
    font.text.setFont(&font.glyphs, m_art.find(sheet));
}

void LegendsDreamcastHudPainter::loadClass(RenderDevice& device, const AssetLocator& files,
                                           s32 classIndex, s32 color) {
    const auto colour = dreamcastPanelColour(color);
    if (!colour || classIndex < 0 || classIndex >= kClassCount) {
        return;
    }
    ClassArt& art = m_classes.at(static_cast<usize>(classIndex)).at(static_cast<usize>(color));
    if (art.taken) {
        return;
    }
    art.taken = true;
    // FUN_8c0ba060 takes a seat's panel from its own costume's bank, and FUN_8c0c08a0 its
    // banner.
    const std::string panel = std::format("{}{}", kPanelPrefix, *colour);
    const std::string_view folderColour = Layout::kColourFolders.at(static_cast<usize>(color));
    const auto code = dreamcastClassCode(classIndex);
    if (code) {
        const std::string folder = costumeFolder(*code, folderColour);
        if (const auto pictures = costumeBank(files, folder)) {
            art.panel = takeCostume(device, *pictures, folder, panel);
            art.banner =
                takeCostume(device, *pictures, folder, std::format("{}{}", *code, kBannerSuffix));
        }
    }
    if (art.panel == nullptr && code != kStandInClass) {
        const std::string folder = costumeFolder(kStandInClass, folderColour);
        if (const auto pictures = costumeBank(files, folder)) {
            art.panel = takeCostume(device, *pictures, folder, panel);
        }
    }
}

const Texture*
LegendsDreamcastHudPainter::takeCostume(RenderDevice& device,
                                        std::span<const formats::LegendsMapTexture> pictures,
                                        const std::string& folder, std::string_view name) {
    const std::string key = std::format("{}/{}", folder, name);
    if (const Texture* kept = m_art.find(key)) {
        return kept;
    }
    const formats::LegendsMapTexture* picture = pictureIn(pictures, name);
    if (picture == nullptr) {
        return nullptr;
    }
    m_art.add(key, upload(device, *picture));
    return m_art.find(key);
}

const LegendsDreamcastHudPainter::ClassArt*
LegendsDreamcastHudPainter::classArtOf(s32 classIndex, s32 color) const {
    if (classIndex < 0 || classIndex >= kClassCount || color < 0 || color >= kColorCount) {
        return nullptr;
    }
    return &m_classes.at(static_cast<usize>(classIndex)).at(static_cast<usize>(color));
}

f32 LegendsDreamcastHudPainter::statusTop() const {
    return static_cast<f32>(Layout::kPlainStripTop);
}

void LegendsDreamcastHudPainter::prepare(HudFrame& frame, const HudView& view) {
    // The parts drawn in Dark Legacy's look are prepared as that look prepares them.
    frame.darkLegacy.look().prepare(frame, view);
    const s64 steps = m_clock.advance(view.seconds);
    const bool rewound = m_clock.rewound();
    for (usize seat = 0; seat < m_glowSteps.size(); ++seat) {
        s64& glowing = m_glowSteps.at(seat);
        // A rewind starts every pulse over, as a box made anew does.
        if (rewound) {
            glowing = 0;
        }
        const std::optional<f32>& turbo = view.seats.at(seat).turbo;
        if (turbo && dreamcastTurbo(*turbo, 0).full) {
            glowing += steps;
        }
    }
}

void LegendsDreamcastHudPainter::drawStatus(HudFrame& frame, const HudView& view) {
    static_assert(HudView::kSeats == Layout::kSeatX.size());
    for (usize seat = 0; seat < view.seats.size(); ++seat) {
        const HudSeat& shown = view.seats.at(seat);
        switch (dreamcastBoxKind(shown)) {
        case LegendsDreamcastBoxKind::None: break;
        case LegendsDreamcastBoxKind::Panel: drawPanel(frame.canvas, seat, shown, view); break;
        case LegendsDreamcastBoxKind::Plain: {
            // A gone player's box keeps its costume's colour, which only its runtime still
            // holds.
            const s32 color = shown.departed && shown.runtime != nullptr
                                  ? shown.runtime->actor.save().color
                                  : shown.box.color;
            drawPlain(frame.canvas, seat, color);
            // FUN_8c0c16e0 tells a player waiting in the tower so, and nobody else.
            if (!shown.departed && shown.box.inTower && !shown.box.towerPrompt) {
                write(frame.canvas, m_small.text, -dreamcastSeatCentre(seat), Layout::kInTowerTop,
                      words(view, kInTowerWord), TextStyle{.color = inkOf(color)});
            }
            break;
        }
        }
    }
}

void LegendsDreamcastHudPainter::drawRuneMeter(HudFrame& frame, const HudView& view) {
    frame.darkLegacy.look().drawRuneMeter(frame, view);
}

void LegendsDreamcastHudPainter::drawHourglass(HudFrame& frame, const HudView& view) {
    frame.darkLegacy.look().drawHourglass(frame, view);
}

void LegendsDreamcastHudPainter::drawBossMeters(HudFrame& frame, const HudView& view) {
    frame.darkLegacy.look().drawBossMeters(frame, view);
}

void LegendsDreamcastHudPainter::drawSelectors(HudFrame& frame, const HudView& view) {
    frame.darkLegacy.look().drawSelectors(frame, view);
}

void LegendsDreamcastHudPainter::drawHelp(HudFrame& frame, const HudView& view) {
    frame.darkLegacy.look().drawHelp(frame, view);
}

void LegendsDreamcastHudPainter::drawPanel(Canvas& canvas, usize seat, const HudSeat& shown,
                                           const HudView& view) const {
    const StatusBoxView& box = shown.box;
    const auto left = static_cast<f32>(dreamcastSeatX(seat) + Layout::kArtLeft);
    // The strip, the costume's panel and the class's banner on it.
    drawWhole(canvas, m_strip, left, static_cast<f32>(Layout::kStripTop));
    const ClassArt* art = classArtOf(box.classIndex, box.color);
    if (art != nullptr) {
        drawPanelArt(canvas, art->panel, left);
    }
    if (art != nullptr && art->banner != nullptr) {
        drawWhole(canvas, art->banner, left, static_cast<f32>(Layout::kBannerTop));
    } else {
        write(canvas, m_large.text, -dreamcastSeatCentre(seat), Layout::kBannerTop,
              classWords(view, box.classIndex), TextStyle{.scale = Layout::kBannerTextScale});
    }
    drawNumbers(canvas, seat, shown);
    drawCarried(canvas, seat, box);
    drawTurbo(canvas, seat, shown);
}

void LegendsDreamcastHudPainter::drawPlain(Canvas& canvas, usize seat, s32 color) const {
    const auto left = static_cast<f32>(dreamcastSeatX(seat) + Layout::kArtLeft);
    drawWhole(canvas, colourPicture(m_plainStrips, color), left,
              static_cast<f32>(Layout::kPlainStripTop));
    drawPanelArt(canvas, colourPicture(m_plainPanels, color), left);
}

void LegendsDreamcastHudPainter::drawNumbers(Canvas& canvas, usize seat,
                                             const HudSeat& shown) const {
    const StatusBoxView& box = shown.box;
    const s32 x = dreamcastSeatX(seat);
    const TextStyle ink{.color = inkOf(box.color)};
    drawWhole(canvas, m_levelLabel, static_cast<f32>(x + Layout::kLevelLabel.x),
              static_cast<f32>(Layout::kLevelLabel.y));
    drawWhole(canvas, m_goldLabel, static_cast<f32>(x + Layout::kGoldLabel.x),
              static_cast<f32>(Layout::kGoldLabel.y));
    drawWhole(canvas, m_healthLabel, static_cast<f32>(x + Layout::kHealthLabel.x),
              static_cast<f32>(Layout::kHealthLabel.y));
    write(canvas, m_small.text, -dreamcastSeatCentre(seat), Layout::kLevelTop,
          std::format("{}", box.level), TextStyle{});
    // FUN_8c0c0f40 writes no gold for a player whose death plays out (states 5 and 6).
    if (standing(shown)) {
        const std::string gold = std::format("{}", std::clamp(box.gold, 0, Layout::kMostGold));
        write(canvas, m_score.text, dreamcastGoldLeft(m_score.text.measure(gold), x),
              Layout::kNumbersTop, gold, ink);
    }
    const std::string health = std::format("{}", std::clamp(box.health, 0, Layout::kMostHealth));
    write(canvas, m_score.text, x + Layout::kHealthRight - m_score.text.measure(health),
          Layout::kNumbersTop, health, ink);
}

void LegendsDreamcastHudPainter::drawCarried(Canvas& canvas, usize seat,
                                             const StatusBoxView& box) const {
    const s32 x = dreamcastSeatX(seat);
    const TextStyle ink{.color = inkOf(box.color)};
    if (box.keys > 0) {
        drawWhole(canvas, m_key, static_cast<f32>(x + Layout::kKeyIcon.x),
                  static_cast<f32>(Layout::kKeyIcon.y));
        write(canvas, m_score.text, x + Layout::kKeyCount.x, Layout::kKeyCount.y,
              std::format("{}", box.keys), ink);
    }
    if (box.potions > 0) {
        drawWhole(canvas, m_art.find(StatusBoxPainter::potionIcon(box.potionKind)),
                  static_cast<f32>(x + Layout::kPotionIcon.x),
                  static_cast<f32>(Layout::kPotionIcon.y));
        write(canvas, m_score.text, x + Layout::kPotionCount.x, Layout::kPotionCount.y,
              std::format("{}", box.potions), ink);
    }
}

void LegendsDreamcastHudPainter::drawTurbo(Canvas& canvas, usize seat, const HudSeat& shown) const {
    if (!shown.turbo) {
        return;
    }
    const s32 x = dreamcastSeatX(seat);
    const LegendsDreamcastTurboArch arch = dreamcastTurbo(*shown.turbo, x);
    if (const Texture* picture = m_art.find(arch.art); picture != nullptr && arch.area.width > 0) {
        canvas.draw(*picture, arch.area, arch.uv, Color::white());
    }
    if (arch.full && m_glow != nullptr) {
        const u8 transparency = dreamcastTurboGlow(m_glowSteps.at(seat));
        canvas.draw(*m_glow,
                    Rect{static_cast<f32>(x + Layout::kArtLeft),
                         static_cast<f32>(Layout::kTurboTop), static_cast<f32>(m_glow->width()),
                         static_cast<f32>(m_glow->height())},
                    Rect{0, 0, 1, 1}, Color::white().withAlpha(kOpaque - transparency));
    }
}

} // namespace gdl::game
