#include "expansions/dreamcast/hud/LegendsDreamcastHudPainter.h"

#include <algorithm>
#include <array>
#include <cmath>
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
#include "game/characters/Inventory.h"
#include "game/characters/Progression.h"
#include "game/hud/BossMeter.h"
#include "game/hud/ChallengeHud.h"
#include "game/hud/HudView.h"
#include "game/hud/PickupHud.h"
#include "game/hud/PowerupSelector.h"
#include "game/hud/RuneMeter.h"
#include "game/hud/StatusBox.h"
#include "game/party/PlayerRuntime.h"
#include "game/players/PowerupEffects.h"

namespace gdl::game {
namespace {

using Layout = LegendsDreamcastHudLayout;

constexpr std::string_view kStrip = "BK_RUNE_STONE_02";
constexpr std::string_view kLevelLabel = "BTMBK_LEVL";
constexpr std::string_view kGoldLabel = "BTMBK_GOLD";
constexpr std::string_view kHealthLabel = "BTMBK_HELTH";
constexpr std::string_view kKeyIcon = "KEY_ICON";
constexpr std::string_view kThirteenthRuneArt = "RUNE13";
constexpr std::string_view kItArt = "IT";
constexpr std::string_view kWaitScrollArt = "TEXTSCROLL";
constexpr std::string_view kThermometerBase = "THERMBASE";
constexpr std::string_view kThermometerColumn = "THERMCOL";
/** The powerups bank's hourglass: its glass, its sand, and its falling grains' frames, by
 * number after this. */
constexpr std::string_view kTimerArt = "TIMER";
constexpr std::string_view kTimerSandArt = "TIMER_SAND";
constexpr std::string_view kGrainsPrefix = "SAND_ANIM";
constexpr std::string_view kPanelPrefix = "BTMBK_";    ///< a costume's panel, by colour
constexpr std::string_view kPlainStripPrefix = "S3_W"; ///< a kind-0 box's strip, by colour
constexpr std::string_view kPlainPanelPrefix = "S4_W"; ///< and its panel
constexpr std::string_view kBannerSuffix = "_NAME";    ///< a class's banner, after its code
constexpr std::string_view kBankObjects = "objects.rom";
constexpr std::string_view kBankTextures = "textures.rom";
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

/** The decoded pictures of the optional bank in `folder`; none when it is not there, and none,
 * with a warning, when it cannot be read. */
std::optional<std::vector<formats::LegendsMapTexture>> optionalBank(const AssetLocator& files,
                                                                    const std::string& folder) {
    const auto objects = files.find(std::format("{}/{}", folder, kBankObjects));
    const auto textures = files.find(std::format("{}/{}", folder, kBankTextures));
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

/** What seat `shown`'s panel shows. A dying player's box, which Dark Legacy's look empties, goes
 * on showing the keys and potions its player carries and the runestones it holds: FUN_8c0c16e0
 * draws them in state 11, and FUN_8c0bc040 drops the keys only once the death has played out,
 * as Dark Legacy does. The bosses' keys stay hidden, as FUN_8c0c2120 hides them then. */
StatusBoxView panelOf(const HudSeat& shown) {
    StatusBoxView box = shown.box;
    if (standing(shown) || shown.runtime == nullptr) {
        return box;
    }
    const ClassProgress& held = shown.runtime->actor.save().progress();
    box.keys = held.inventory.keys;
    box.potions = static_cast<s32>(held.inventory.potions.size());
    box.potionKind = held.inventory.nextPotion();
    box.runes = held.relics.runes;
    return box;
}

/** Seat `shown`'s costume colour: a gone player's, which only its runtime still holds, else its
 * box's. */
s32 seatColour(const HudSeat& shown) {
    return shown.departed && shown.runtime != nullptr ? shown.runtime->actor.save().color
                                                      : shown.box.color;
}

/** The static set's picture of runestone `rune`, three of each colour (FUN_8c0ba060's
 * SM_RUNE_%s_%02d). */
std::string runeArt(usize rune) {
    return std::format("SM_RUNE_{}_{:02}",
                       StatusBoxPainter::kRelicColours.at(
                           rune / static_cast<usize>(StatusBoxPainter::kRunesInColour)),
                       rune % static_cast<usize>(StatusBoxPainter::kRunesInColour) + 1);
}

/** Whether `bits` holds bit `bit`. */
bool held(u16 bits, usize bit) {
    return (bits & (1U << static_cast<u32>(bit))) != 0;
}

/** `place`, a square of the box, taken in to `picture`'s shape about its middle: a picture of
 * Dark Legacy's fitted into a place of the Dreamcast's. */
Rect fittedInto(const Rect& place, const Texture& picture) {
    const auto width = static_cast<f32>(picture.width());
    const auto height = static_cast<f32>(picture.height());
    if (width <= 0 || height <= 0) {
        return place;
    }
    const f32 scale = std::min(place.width / width, place.height / height);
    const f32 across = width * scale;
    const f32 down = height * scale;
    return {place.x + (place.width - across) / 2, place.y + (place.height - down) / 2, across,
            down};
}

} // namespace

bool LegendsDreamcastHudPainter::load(RenderDevice& device, const std::filesystem::path& root,
                                      DarkLegacyHudArt& darkLegacy,
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
    // The powerups bank holds both the legend items' icons and the hourglass: it is read once.
    const auto powerups = optionalBank(files, std::string{kPowerupsFolder});
    std::span<const formats::LegendsMapTexture> powerupPictures;
    if (powerups) {
        powerupPictures = *powerups;
    }
    loadLegends(device, powerupPictures, darkLegacy);
    if (powerups) {
        loadTimer(device, powerupPictures);
    }
    borrowPictures(darkLegacy);
    return true;
}

void LegendsDreamcastHudPainter::release() {
    for (Font* font : {&m_small, &m_score, &m_large}) {
        font->text.setFont(nullptr, nullptr);
        font->glyphs = {};
    }
    m_strip = m_levelLabel = m_goldLabel = m_healthLabel = m_key = m_glow = nullptr;
    m_thirteenthRune = m_it = m_waitScroll = nullptr;
    m_thermometerBase = m_thermometerColumn = m_timer = m_timerSand = nullptr;
    m_grains = {};
    m_helpScrolls = {};
    m_darkLegacyCards.clear();
    m_countIcons.clear();
    m_plainStrips = {};
    m_plainPanels = {};
    m_runes = {};
    m_keys = {};
    m_legends = {};
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
    // Every picture the look draws is taken whole before it is drawn; nothing else of the set is
    // uploaded.
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
    for (usize rune = 0; rune < m_runes.size(); ++rune) {
        m_runes.at(rune) = take(runeArt(rune));
    }
    // FUN_8c0ba060's SM_KEY_%s: Dark Legacy's eight keys share the Dreamcast's four pictures, by
    // the colours Dark Legacy gives them.
    for (const std::string_view colour : StatusBoxPainter::kRelicColours) {
        const Texture* picture = take(std::format("SM_KEY_{}", colour));
        for (usize key = 0; key < m_keys.size(); ++key) {
            if (StatusBoxPainter::kKeyColours.at(key) == colour) {
                m_keys.at(key) = picture;
            }
        }
    }
    m_thirteenthRune = take(kThirteenthRuneArt);
    m_it = take(kItArt);
    m_waitScroll = take(kWaitScrollArt);
    m_thermometerBase = take(kThermometerBase);
    m_thermometerColumn = take(kThermometerColumn);
    // The help's plain scroll is WAIT HERE's.
    for (usize colour = 0; colour < m_helpScrolls.size(); ++colour) {
        const std::string_view scroll = Layout::kHelpScrolls.at(colour);
        const Texture* taken = m_art.find(scroll);
        m_helpScrolls.at(colour) = taken != nullptr ? taken : take(scroll);
    }
    // FUN_8c0b7560's six cards, each standing for one or more of Dark Legacy's.
    for (const LegendsDreamcastCardKind& kind : Layout::kCards) {
        if (!kind.face.empty() && m_art.find(kind.face) == nullptr) {
            take(kind.face);
        }
    }
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
        if (const auto pictures = optionalBank(files, folder)) {
            art.panel = takePicture(device, *pictures, folder, panel);
            art.banner =
                takePicture(device, *pictures, folder, std::format("{}{}", *code, kBannerSuffix));
        }
    }
    if (art.panel == nullptr && code != kStandInClass) {
        const std::string folder = costumeFolder(kStandInClass, folderColour);
        if (const auto pictures = optionalBank(files, folder)) {
            art.panel = takePicture(device, *pictures, folder, panel);
        }
    }
}

void LegendsDreamcastHudPainter::loadLegends(RenderDevice& device,
                                             std::span<const formats::LegendsMapTexture> pictures,
                                             DarkLegacyHudArt& darkLegacy) {
    // FUN_8c0c08a0 gives the Dreamcast's five legend items' places QUEST_ICON1..5, which the
    // powerups bank holds. Dark Legacy's own picture stands in for an item the Dreamcast has none
    // of, or one the bank does not hold.
    const std::string folder{kPowerupsFolder};
    for (const LegendsDreamcastLegend& legend : Layout::kLegends) {
        LegendArt& art = m_legends.at(static_cast<usize>(legend.realm));
        if (!legend.icon.empty()) {
            art.picture = takePicture(device, pictures, folder, legend.icon);
        }
        if (art.picture == nullptr) {
            art.picture = darkLegacy.selectArt(legend.picture);
            art.fitted = true;
        }
    }
}

void LegendsDreamcastHudPainter::loadTimer(RenderDevice& device,
                                           std::span<const formats::LegendsMapTexture> pictures) {
    // FUN_8c09b640's glass, its sand and the grains' frames. The hourglass is drawn whole or not
    // at all: every picture is found before any is taken.
    std::array<std::string, Layout::kSandFrames> grains;
    for (usize frame = 0; frame < grains.size(); ++frame) {
        grains.at(frame) = std::format("{}{:02}", kGrainsPrefix, frame);
    }
    std::vector<std::string_view> names{kTimerArt, kTimerSandArt};
    names.insert(names.end(), grains.begin(), grains.end());
    for (const std::string_view name : names) {
        if (pictureIn(pictures, name) == nullptr) {
            log::warn("Dreamcast HUD: {}: no {}; the hourglass is Dark Legacy's", kPowerupsFolder,
                      name);
            return;
        }
    }
    const std::string folder{kPowerupsFolder};
    m_timer = takePicture(device, pictures, folder, kTimerArt);
    m_timerSand = takePicture(device, pictures, folder, kTimerSandArt);
    for (usize frame = 0; frame < grains.size(); ++frame) {
        m_grains.at(frame) = takePicture(device, pictures, folder, grains.at(frame));
    }
}

void LegendsDreamcastHudPainter::borrowPictures(DarkLegacyHudArt& darkLegacy) {
    for (const LegendsDreamcastCardKind& kind : Layout::kCards) {
        if (!kind.face.empty()) {
            continue;
        }
        if (const Texture* picture = darkLegacy.staticArt(kind.card)) {
            m_darkLegacyCards.emplace(std::string{kind.card}, picture);
        }
    }
    for (std::string& icon : PickupHud::countIcons()) {
        if (const Texture* picture = darkLegacy.countArt(icon)) {
            m_countIcons.emplace(std::move(icon), picture);
        }
    }
}

const Texture*
LegendsDreamcastHudPainter::takePicture(RenderDevice& device,
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
    // Without its own art the hourglass is Dark Legacy's, prepared as that look prepares it.
    if (m_timer == nullptr) {
        frame.darkLegacy.look().prepare(frame, view);
    }
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
    Canvas& canvas = frame.canvas;
    for (usize seat = 0; seat < view.seats.size(); ++seat) {
        const HudSeat& shown = view.seats.at(seat);
        const LegendsDreamcastBoxKind kind = dreamcastBoxKind(shown);
        if (kind == LegendsDreamcastBoxKind::None) {
            continue;
        }
        const bool panel = kind == LegendsDreamcastBoxKind::Panel;
        if (panel) {
            drawPanel(canvas, seat, shown, view);
        } else {
            drawPlain(canvas, seat, seatColour(shown));
        }
        if (shown.it) {
            drawIt(canvas, seat);
        }
        if (panel) {
            drawWaitHere(canvas, seat, shown, view);
        } else {
            drawFallen(canvas, seat, shown, view);
        }
    }
    // FUN_8c0b76a0's cards rise over every box, and Dark Legacy's counts stand over them.
    drawCards(canvas, view);
    drawCounts(canvas, view);
}

void LegendsDreamcastHudPainter::drawRuneMeter(HudFrame& frame, const HudView& view) {
    if (!view.runeFill || m_thermometerBase == nullptr || m_thermometerColumn == nullptr) {
        return;
    }
    // The base, then its column over it, as their blits' depths stand them (FUN_8c09b360).
    drawWhole(frame.canvas, m_thermometerBase, static_cast<f32>(Layout::kThermometerLeft),
              static_cast<f32>(Layout::kThermometerTop));
    const RuneMeter::Column column =
        dreamcastThermometerColumn(*view.runeFill, static_cast<f32>(m_thermometerColumn->width()));
    frame.canvas.draw(*m_thermometerColumn, column.area, column.uv, Color::white());
}

void LegendsDreamcastHudPainter::drawHourglass(HudFrame& frame, const HudView& view) {
    if (m_timer == nullptr) {
        frame.darkLegacy.look().drawHourglass(frame, view);
        return;
    }
    if (!view.hourglass || !std::isfinite(view.hourglass->elapsed)) {
        return;
    }
    // FUN_8c09b640's blits, deepest first: the glass, the grains while the clock runs, then the
    // sand each bulb holds.
    Canvas& canvas = frame.canvas;
    canvas.draw(*m_timer, ChallengeHud::kFrameArea);
    if (view.hourglass->fallingFrame >= 0) {
        const auto frameShown = static_cast<usize>(view.hourglass->fallingFrame) % m_grains.size();
        if (const Texture* grains = m_grains.at(frameShown)) {
            canvas.draw(*grains, ChallengeHud::kFallingArea);
        }
    }
    const ChallengeHud::Sand sand =
        ChallengeHud::sandAt(std::clamp(view.hourglass->elapsed, 0.0f, 1.0f));
    canvas.draw(*m_timerSand, sand.upper, sand.upperUv, Color::white());
    canvas.draw(*m_timerSand, sand.lower, sand.lowerUv, Color::white());
}

void LegendsDreamcastHudPainter::drawBossMeters(HudFrame& frame, const HudView& view) {
    if (view.bossMeters == nullptr) {
        return;
    }
    const BossMeters& meters = *view.bossMeters;
    const Color shade = Color::white().withAlpha(Layout::kMeterOpacity);
    const auto top = static_cast<f32>(Layout::kMeterTop);
    const auto strip = static_cast<f32>(BossMeter::kPieceWidth);
    for (usize index = 0; index < meters.count(); ++index) {
        const BossMeter& meter = meters.meter(index);
        if (!meter.showing()) {
            continue;
        }
        const BossMeter::Layout layout = meter.layout();
        const BossMeter::Look look = meter.look();
        const s32 pieces = std::min(layout.pieces, BossMeter::kMostPieces);
        for (s32 piece = 0; piece < pieces; ++piece) {
            const f32 x = static_cast<f32>(layout.left) + (strip * static_cast<f32>(piece));
            if (const Texture* back =
                    meter.pieceTexture(frame.device, BossMeter::MeterPart::Background, piece)) {
                frame.canvas.draw(*back, Rect{x, top, strip, static_cast<f32>(back->height())},
                                  shade);
            }
            const s32 width =
                std::clamp(look.widths.at(static_cast<usize>(piece)), 0, BossMeter::kPieceWidth);
            if (width <= 0) {
                continue;
            }
            if (const Texture* fill =
                    meter.pieceTexture(frame.device, BossMeter::MeterPart::Fill, piece)) {
                // Cropped, not squeezed: the fill's left part.
                const auto across = static_cast<f32>(width);
                frame.canvas.draw(*fill, Rect{x, top, across, static_cast<f32>(fill->height())},
                                  Rect{0, 0, across / strip, 1}, shade);
            }
        }
    }
}

void LegendsDreamcastHudPainter::drawSelectors(HudFrame& frame, const HudView& view) {
    if (!m_small.text.ready()) {
        return;
    }
    // Dark Legacy's label writes FONT32 at its own size from the selector's label top; this one
    // keeps its bottom there.
    const s32 darkLegacyLine =
        view.largeText != nullptr ? view.largeText->lineHeight(PowerupSelector::kLabelScale) : 0;
    for (usize seat = 0; seat < view.seats.size(); ++seat) {
        const HudSeat& shown = view.seats.at(seat);
        const PowerupSelector* selector = shown.selector;
        if (selector == nullptr || shown.runtime == nullptr || !selector->showing() ||
            selector->selection() < 0) {
            continue;
        }
        const Inventory& carried = shown.runtime->actor.save().progress().inventory;
        const auto chosen = static_cast<usize>(selector->selection());
        if (chosen >= carried.powerups.size()) {
            continue;
        }
        const PowerupSlot& slot = carried.powerups.at(chosen);
        const std::string_view label = words(view, powerupTextId(slot.kind, slot.flags));
        const f32 scale = dreamcastLabelScale(m_small.text, label);
        const s32 bottom = selector->labelY(StatusBoxPainter::kY) + darkLegacyLine;
        write(frame.canvas, m_small.text, -dreamcastSeatCentre(seat),
              bottom - m_small.text.lineHeight(scale), label,
              TextStyle{.scale = scale,
                        .color = slot.on ? inkOf(seatColour(shown)) : Color::white()});
    }
}

void LegendsDreamcastHudPainter::drawHelp(HudFrame& frame, const HudView& view) {
    if (!view.help || view.help->lines.empty()) {
        return;
    }
    const HudHelp& help = *view.help;
    const LegendsDreamcastHelpBox box =
        dreamcastHelpBox(help.lines, help.head, help.anchored, help.player);
    const usize colour = dreamcastHelpScroll(help.color);
    if (const Texture* scroll = m_helpScrolls.at(colour)) {
        frame.canvas.draw(*scroll,
                          Rect{static_cast<f32>(box.x), static_cast<f32>(box.y),
                               static_cast<f32>(box.width), static_cast<f32>(box.height)},
                          Color::white().withAlpha(kOpaque - Layout::kHelpTransparency));
    }
    const TextStyle ink{.color = Layout::kHelpInks.at(colour)};
    s32 top = box.textTop;
    for (const std::string& line : help.lines) {
        write(frame.canvas, m_small.text, -box.textCentre, top, line, ink);
        top += Layout::kHelpLineHeight;
    }
}

void LegendsDreamcastHudPainter::drawPanel(Canvas& canvas, usize seat, const HudSeat& shown,
                                           const HudView& view) const {
    const StatusBoxView box = panelOf(shown);
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
    drawNumbers(canvas, seat, box);
    drawCarried(canvas, seat, box);
    drawTurbo(canvas, seat, shown);
    drawRelics(canvas, seat, box, view.relicsShown);
    // FUN_8c0c16e0 shows the legend items only to a player playing or waiting at an exit.
    if (standing(shown)) {
        drawLegends(canvas, seat, shown.legends);
    }
}

void LegendsDreamcastHudPainter::drawPlain(Canvas& canvas, usize seat, s32 color) const {
    const auto left = static_cast<f32>(dreamcastSeatX(seat) + Layout::kArtLeft);
    drawWhole(canvas, colourPicture(m_plainStrips, color), left,
              static_cast<f32>(Layout::kPlainStripTop));
    drawPanelArt(canvas, colourPicture(m_plainPanels, color), left);
}

void LegendsDreamcastHudPainter::drawNumbers(Canvas& canvas, usize seat,
                                             const StatusBoxView& box) const {
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
    // FUN_8c0c0f40 writes the gold for a dying player too (state 11); it leaves it out only in
    // states 5 and 6, which state 3 leads to (8c0bf120..8c0bf16e), not a death.
    const std::string gold = std::format("{}", std::clamp(box.gold, 0, Layout::kMostGold));
    write(canvas, m_score.text, dreamcastGoldLeft(m_score.text.measure(gold), x),
          Layout::kNumbersTop, gold, ink);
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

void LegendsDreamcastHudPainter::drawRelics(Canvas& canvas, usize seat, const StatusBoxView& box,
                                            bool keysShown) const {
    const s32 x = dreamcastSeatX(seat);
    for (usize rune = 0; rune < m_runes.size(); ++rune) {
        if (held(box.runes, rune)) {
            const auto index = static_cast<s32>(rune);
            const s32 trio = index / StatusBoxPainter::kRunesInColour;
            const s32 left = x + Layout::kRuneLeft + (index * Layout::kRuneStep) + trio;
            drawWhole(canvas, m_runes.at(rune), static_cast<f32>(left),
                      static_cast<f32>(Layout::kRuneTop));
        }
    }
    // Dark Legacy's showing of its relics stands for the Dreamcast's relic timer.
    if (keysShown) {
        for (usize key = 0; key < m_keys.size(); ++key) {
            if (held(box.bossKeys, key)) {
                drawWhole(canvas, m_keys.at(key),
                          static_cast<f32>(x + Layout::kKeyLeft +
                                           (static_cast<s32>(key) * Layout::kKeyStep)),
                          static_cast<f32>(Layout::kKeyTop));
            }
        }
    }
    if ((box.runes & StatusBoxPainter::kThirteenthRune) != 0) {
        drawWhole(canvas, m_thirteenthRune, static_cast<f32>(x + Layout::kThirteenthRune.x),
                  static_cast<f32>(Layout::kThirteenthRune.y));
    }
}

void LegendsDreamcastHudPainter::drawLegends(Canvas& canvas, usize seat, u16 legends) const {
    const s32 x = dreamcastSeatX(seat);
    const auto places = dreamcastQuestPlaces(legends);
    for (usize place = 0; place < places.size(); ++place) {
        const std::optional<s32>& realm = places.at(place);
        if (!realm) {
            continue;
        }
        const LegendArt& art = m_legends.at(static_cast<usize>(*realm));
        if (art.picture == nullptr) {
            continue;
        }
        const LegendsDreamcastPoint at = Layout::kQuestPlaces.at(place);
        const Rect square{static_cast<f32>(x + at.x), static_cast<f32>(at.y),
                          static_cast<f32>(Layout::kQuestIconSize),
                          static_cast<f32>(Layout::kQuestIconSize)};
        canvas.draw(*art.picture, art.fitted ? fittedInto(square, *art.picture) : square);
    }
}

void LegendsDreamcastHudPainter::drawIt(Canvas& canvas, usize seat) const {
    // FUN_8c0c2300 stands the badge over the tagged player's box, whatever the box shows.
    if (m_it != nullptr) {
        canvas.draw(*m_it, Rect{static_cast<f32>(dreamcastSeatX(seat) + Layout::kItBadge.x),
                                static_cast<f32>(Layout::kItBadge.y),
                                static_cast<f32>(Layout::kItBadgeSize),
                                static_cast<f32>(Layout::kItBadgeSize)});
    }
}

void LegendsDreamcastHudPainter::drawWaitHere(Canvas& canvas, usize seat, const HudSeat& shown,
                                              const HudView& view) const {
    if (!dreamcastWaitHereShown(shown.waitingAtExit, view.joined, m_clock.step())) {
        return;
    }
    // The scroll and its words together: one never shows without the other.
    const s32 x = dreamcastSeatX(seat);
    if (m_waitScroll != nullptr) {
        canvas.draw(*m_waitScroll, Rect{static_cast<f32>(x + Layout::kWaitScroll.x),
                                        static_cast<f32>(Layout::kWaitScroll.y),
                                        static_cast<f32>(Layout::kWaitScrollWidth),
                                        static_cast<f32>(Layout::kWaitScrollRows)});
    }
    write(canvas, m_small.text, x + Layout::kWaitHereLine.x, Layout::kWaitHereLine.y,
          words(view, Layout::kWaitHereText), TextStyle{.color = Layout::kWaitHereInk});
}

void LegendsDreamcastHudPainter::drawFallen(Canvas& canvas, usize seat, const HudSeat& shown,
                                            const HudView& view) const {
    // A gone player's box says nothing more (FUN_8c0bbee0).
    if (shown.departed) {
        return;
    }
    const s32 centre = -dreamcastSeatCentre(seat);
    const TextStyle ink{.color = inkOf(seatColour(shown))};
    if (shown.box.towerPrompt) {
        // Dark Legacy's prompt has no cursor, so YES, where the original's starts, stays
        // marked; NO is offered only while the player may quit the game.
        write(canvas, m_small.text, centre, Layout::kPromptWaitTop, words(view, Layout::kWaitText),
              ink);
        write(canvas, m_small.text, centre, Layout::kPromptQuestionTop,
              words(view, Layout::kInTowerQuestionText), ink);
        write(canvas, m_small.text, centre, Layout::kPromptYesTop, words(view, Layout::kYesText),
              TextStyle{.color = Layout::kPromptChosenInk});
        if (shown.box.quitOffered) {
            write(canvas, m_small.text, centre, Layout::kPromptNoTop, words(view, Layout::kNoText),
                  ink);
        }
        return;
    }
    // FUN_8c0c16e0 tells a player waiting in the tower so.
    if (shown.box.inTower) {
        write(canvas, m_small.text, centre, Layout::kInTowerTop, words(view, Layout::kInTowerText),
              ink);
    }
}

void LegendsDreamcastHudPainter::drawCards(Canvas& canvas, const HudView& view) const {
    if (view.pickups == nullptr) {
        return;
    }
    for (const PickupCard& card : view.pickups->cards()) {
        const auto shown = dreamcastCard(card.texture);
        if (!shown || card.player < 0 || static_cast<usize>(card.player) >= view.seats.size()) {
            continue;
        }
        const auto seat = static_cast<usize>(card.player);
        const auto left = static_cast<f32>(dreamcastSeatX(seat) + Layout::kArtLeft);
        drawWhole(canvas, colourPicture(m_plainStrips, seatColour(view.seats.at(seat))), left,
                  static_cast<f32>(card.y));
        const auto top = static_cast<f32>(card.y + Layout::kCardStripRows);
        if (!shown->darkLegacy) {
            drawWhole(canvas, m_art.find(shown->art), left, top);
            continue;
        }
        // Dark Legacy's card, as its own boxes size it.
        if (const auto lent = m_darkLegacyCards.find(shown->art); lent != m_darkLegacyCards.end()) {
            canvas.draw(*lent->second, Rect{left, top, static_cast<f32>(StatusBoxPainter::kWidth),
                                            static_cast<f32>(StatusBoxPainter::kHeight)});
        }
    }
}

void LegendsDreamcastHudPainter::drawCounts(Canvas& canvas, const HudView& view) const {
    if (view.pickups == nullptr) {
        return;
    }
    const TextStyle style{.scale = Layout::kTickerScale};
    const s32 rowStep = StatusBoxPainter::kCountIconSize + StatusBoxPainter::kCountRowGap;
    const auto icon = static_cast<f32>(StatusBoxPainter::kCountIconSize);
    for (usize seat = 0; seat < view.seats.size(); ++seat) {
        // Dark Legacy's rows: up from over the box, or from above its selector's label.
        std::optional<s32> labelTop;
        const PowerupSelector* selector = view.seats.at(seat).selector;
        if (selector != nullptr && selector->showing()) {
            labelTop = selector->labelY(StatusBoxPainter::kY);
        }
        const s32 bottom = PickupHud::countRowY(labelTop);
        s32 line = 0;
        for (const PickupCount& held : view.pickups->counts(static_cast<s32>(seat))) {
            if (!held.showing()) {
                continue;
            }
            const std::string text = std::format("{}/{}", held.count, held.total);
            const s32 width = StatusBoxPainter::kCountIconSize + StatusBoxPainter::kCountIconGap +
                              m_small.text.measure(text, style.scale);
            const s32 left = dreamcastSeatCentre(seat) - (width / 2);
            const s32 top = bottom - (line * rowStep);
            ++line;
            if (const auto found = m_countIcons.find(held.icon); found != m_countIcons.end()) {
                canvas.draw(*found->second,
                            Rect{static_cast<f32>(left), static_cast<f32>(top), icon, icon});
            }
            write(canvas, m_small.text,
                  left + StatusBoxPainter::kCountIconSize + StatusBoxPainter::kCountIconGap,
                  top + StatusBoxPainter::kCountTextDy, text, style);
        }
    }
}

} // namespace gdl::game
