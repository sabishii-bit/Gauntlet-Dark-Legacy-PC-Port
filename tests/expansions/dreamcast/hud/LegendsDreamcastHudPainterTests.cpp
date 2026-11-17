#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/BitmapFont.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/StringTable.h"
#include "engine/assets/TextureSet.h"
#include "engine/core/Error.h"
#include "engine/core/SpecialMembers.h"
#include "engine/core/Strings.h"
#include "engine/core/Types.h"
#include "engine/math/Math.h"
#include "engine/render/Image.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "FakeDarkLegacyArt.h"
#include "FakeHudStyles.h"
#include "FakeRenderDevice.h"
#include "LogCapture.h"
#include "RecordedDrawChecks.h"
#include "RecordedQuads.h"
#include "TestSupport.h"
#include "expansions/dreamcast/hud/LegendsDreamcastHudFixture.h"
#include "expansions/dreamcast/hud/LegendsDreamcastHudLayout.h"
#include "expansions/dreamcast/hud/LegendsDreamcastHudPainter.h"
#include "expansions/shared/hud/LegendsHudClock.h"
#include "expansions/shared/hud/LegendsHudStyleIds.h"
#include "formats/AnimationTree.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/ClassData.h"
#include "game/characters/Inventory.h"
#include "game/characters/Relics.h"
#include "game/combat/ArmorFlags.h"
#include "game/combat/PowerupFlags.h"
#include "game/config/GameConfig.h"
#include "game/hud/BossMeter.h"
#include "game/hud/ChallengeHud.h"
#include "game/hud/DarkLegacyHudPainter.h"
#include "game/hud/HealthMeter.h"
#include "game/hud/HudPainter.h"
#include "game/hud/HudStyles.h"
#include "game/hud/HudView.h"
#include "game/hud/PickupHud.h"
#include "game/hud/PowerupSelector.h"
#include "game/hud/RuneMeter.h"
#include "game/hud/StatusBox.h"
#include "game/party/PlayerRuntime.h"
#include "game/players/PlayerControls.h"

namespace {

using namespace gdl;
using namespace gdl::game;
using Fixture = test::LegendsDreamcastHudFixture;
using Layout = LegendsDreamcastHudLayout;
using Names = std::vector<std::string_view>;
using test::checkRect;
using test::Quad;
using test::quadsOf;

/** Full health at the level the cases' characters stand at. */
constexpr s32 kMostHealth = 600;
/** A class's name where it has no banner: font32 at half its size. */
constexpr f32 kHalf = 0.5f;
/** Each costume colour's ink, yellow, blue, red and green. */
constexpr Color kYellowInk = Layout::kTextColours[0];
constexpr Color kBlueInk = Layout::kTextColours[1];
constexpr Color kRedInk = Layout::kTextColours[2];
constexpr Color kGreenInk = Layout::kTextColours[3];

/** The seat whose box a quad stands in, by its left edge: the last whose art starts at or
 * before it. */
usize seatOf(const Quad& quad) {
    usize seat = 0;
    while (seat + 1 < Layout::kSeatX.size() &&
           quad.area.x >= static_cast<f32>(Layout::kSeatX.at(seat + 1) + Layout::kArtLeft)) {
        ++seat;
    }
    return seat;
}

/** Whether a quad is a glyph of one of the fixture's fonts. */
bool isText(const Quad& quad) {
    return Fixture::sheetOf(quad.texture, Fixture::kSmallFont) ||
           Fixture::sheetOf(quad.texture, Fixture::kScoreFont) ||
           Fixture::sheetOf(quad.texture, Fixture::kLargeFont);
}

/** The quads of picture `name` in `seat`, in turn; any seat's when none is named. */
std::vector<Quad> picturesIn(std::span<const Quad> quads, std::string_view name,
                             std::optional<usize> seat = std::nullopt) {
    std::vector<Quad> found;
    for (const Quad& quad : quads) {
        if (!isText(quad) && Fixture::pictureOf(quad.texture).name == name &&
            (!seat || seatOf(quad) == *seat)) {
            found.push_back(quad);
        }
    }
    return found;
}

/** The names of the pictures standing in `seat`, in the order drawn, the glyphs left out. */
Names pictureOrder(std::span<const Quad> quads, usize seat) {
    Names names;
    for (const Quad& quad : quads) {
        const std::string_view name = Fixture::pictureOf(quad.texture).name;
        if (!name.empty() && !isText(quad) && seatOf(quad) == seat) {
            names.push_back(name);
        }
    }
    return names;
}

/** The folder of the bank picture `name` of `seat` came from; empty when it is not drawn. */
std::string_view folderOf(std::span<const Quad> quads, std::string_view name, usize seat) {
    const std::vector<Quad> found = picturesIn(quads, name, seat);
    return found.empty() ? std::string_view{} : Fixture::pictureOf(found.front().texture).folder;
}

/** A line one of the fixture's fonts wrote: its first glyph's place, its characters, read back
 * from where the glyphs were cut from the sheet, a space where the pen skipped one, and its
 * colour. */
struct Line {
    f32 x = 0;
    f32 y = 0;
    std::string text;
    Color color{};
    bool operator==(const Line&) const = default;
};

/** How far a pen writing `text` in `font` at `scale` moves, as TextPainter measures it. */
s32 measure(const test::DreamcastFont& font, std::string_view text, f32 scale = 1) {
    s32 width = 0;
    for (const char code : text) {
        const s32 cell = code == ' '                                  ? font.space
                         : font.codes.find(code) != std::string::npos ? font.columns
                                                                      : 0;
        width += static_cast<s32>(static_cast<f32>(cell) * scale);
    }
    return width;
}

/** The lines `font` wrote at `scale`, in turn: each a run of its glyphs, one after another, along
 * one top. */
std::vector<Line> linesOf(std::span<const Quad> quads, const test::DreamcastFont& font,
                          f32 scale = 1) {
    const auto advance = static_cast<f32>(static_cast<s32>(static_cast<f32>(font.columns) * scale));
    const auto space = static_cast<f32>(static_cast<s32>(static_cast<f32>(font.space) * scale));
    std::vector<Line> lines;
    bool continuing = false;
    f32 previous = 0;
    for (const Quad& quad : quads) {
        if (!Fixture::sheetOf(quad.texture, font)) {
            continuing = false;
            continue;
        }
        // TextPainter samples a glyph from half a texel inside its cell.
        const auto cell =
            static_cast<usize>(std::lround((quad.uv.x * static_cast<f32>(font.sheet.width) - 0.5f) /
                                           static_cast<f32>(font.columns)));
        const char code = font.codes.at(cell);
        const f32 gap = quad.area.x - previous;
        if (continuing && !lines.empty() && lines.back().y == quad.area.y &&
            (gap == advance || gap == advance + space)) {
            if (gap != advance) {
                lines.back().text.push_back(' ');
            }
        } else {
            lines.push_back({quad.area.x, quad.area.y, {}, quad.color});
        }
        lines.back().text.push_back(code);
        previous = quad.area.x;
        continuing = true;
    }
    return lines;
}

/** Where a line written at `scale` from pen point (x, y) lands: half a texel in. */
Line lineAt(s32 x, s32 y, std::string text, Color color, f32 scale = 1) {
    const f32 inset = 0.5f * scale;
    return {static_cast<f32>(x) + inset, static_cast<f32>(y) + inset, std::move(text), color};
}

/** A line in `font` at `scale` centred on `centre`, as TextPainter centres it: half its width,
 * rounded down, before the centre. */
Line centredAt(const test::DreamcastFont& font, s32 centre, s32 y, std::string text, Color color,
               f32 scale = 1) {
    const s32 left = centre - measure(font, text, scale) / 2;
    return lineAt(left, y, std::move(text), color, scale);
}

/** A standing player's seat: class, costume colour and name, `health` of kMostHealth, level 12,
 * 1250 gold, the turbo meter showing nothing. */
HudSeat standing(s32 classIndex, s32 color, std::string name = "A", s32 health = kMostHealth) {
    HudSeat seat;
    seat.box.mode = StatusBoxView::Mode::Status;
    seat.box.active = true;
    seat.box.classIndex = classIndex;
    seat.box.color = color;
    seat.box.name = std::move(name);
    seat.box.level = 12;
    seat.box.gold = 1250;
    seat.box.health = health;
    seat.mostHealth = kMostHealth;
    seat.turbo = 0.0f;
    return seat;
}

/** `seat` fallen, as PartyHud::status shows it: health gone and nothing carried, no relics
 * held; in the tower, asked to wait there when `prompt`, or else still dying. The legend items
 * held stay, as hudSeatOf reads them for every player. */
HudSeat fallen(HudSeat seat, bool inTower, bool prompt = false) {
    seat.box.health = 0;
    seat.box.keys = 0;
    seat.box.potions = 0;
    seat.box.potionKind = 0;
    seat.box.runes = 0;
    seat.box.bossKeys = 0;
    seat.box.inTower = inTower;
    seat.box.towerPrompt = inTower && prompt;
    seat.turbo.reset();
    return seat;
}

/** The legend items of `realms`, a bit per realm, as Relics keeps them. */
u16 legendsOf(std::initializer_list<s32> realms) {
    u16 legends = 0;
    for (const s32 realm : realms) {
        legends = static_cast<u16>(legends | (1U << static_cast<u32>(realm)));
    }
    return legends;
}

/** A picture of Dark Legacy's art as the cases lend it: its set's name and its size. */
struct LentPicture {
    std::string_view name;
    u32 width = 0;
    u32 height = 0;
};
/** SELECT's pictures of the legend items, in realm order, each of its own shape, so that how one
 * is fitted into a place of the box shows. */
constexpr std::array<LentPicture, 9> kLentLegends{{
    {.name = "SCIMITAR", .width = 32, .height = 16},
    {.name = "ICE_AX", .width = 16, .height = 32},
    {.name = "LAMP", .width = 24, .height = 24},
    {.name = "BILLOWS", .width = 40, .height = 20},
    {.name = "SOUL_SAVIOR", .width = 20, .height = 40},
    {.name = "BOOK", .width = 32, .height = 24},
    {.name = "FIRE_SCROLL", .width = 24, .height = 48},
    {.name = "LANTERN", .width = 8, .height = 8},
    {.name = "JAVILIN", .width = 64, .height = 16},
}};

/** Dark Legacy's art lending `kLentLegends`: each picture by its name. */
struct LentLegends {
    std::vector<std::pair<std::string_view, const Texture*>> pictures;

    explicit LentLegends(test::FakeDarkLegacyArt& art) {
        for (const LentPicture& legend : kLentLegends) {
            pictures.emplace_back(legend.name, &art.give(test::DarkLegacyLookup::Set::Select,
                                                         legend.name, legend.width, legend.height));
        }
    }
    const Texture* operator()(std::string_view name) const {
        for (const auto& [lentName, picture] : pictures) {
            if (lentName == name) {
                return picture;
            }
        }
        return nullptr;
    }
    /** The quads of lent picture `name`. */
    std::vector<Quad> drawn(std::span<const Quad> quads, std::string_view name) const {
        std::vector<Quad> found;
        for (const Quad& quad : quads) {
            if (quad.texture == (*this)(name)) {
                found.push_back(quad);
            }
        }
        return found;
    }
    /** The name of lent picture `texture`; empty for none of these. */
    std::string_view nameOf(const Texture* texture) const {
        for (const auto& [name, picture] : pictures) {
            if (picture == texture) {
                return name;
            }
        }
        return {};
    }
};

/** The names of the pictures standing in `seat`, in the order drawn, the glyphs left out: the
 * extraction's, and the legend items `lent` of Dark Legacy's. */
Names boxOrder(std::span<const Quad> quads, usize seat, const LentLegends& lent) {
    Names names;
    for (const Quad& quad : quads) {
        if (isText(quad) || seatOf(quad) != seat) {
            continue;
        }
        const std::string_view own = Fixture::pictureOf(quad.texture).name;
        const std::string_view name = own.empty() ? lent.nameOf(quad.texture) : own;
        if (!name.empty()) {
            names.push_back(name);
        }
    }
    return names;
}

/** A player who quit the game from the prompt in seat `seat`: out of the party, its box left
 * empty and its runtime kept, as hudSeatOf shows it. */
struct Departed {
    PlayerRuntime runtime;

    Departed(s32 seat, s32 classIndex, s32 color, std::string name) {
        CharacterSave save;
        save.name = std::move(name);
        save.character = classIndex;
        save.color = color;
        runtime.actor.spawn(seat, save, nullptr, Vec3{0}, 0);
        runtime.departed = true;
    }
    HudSeat seat() const {
        HudSeat shown;
        shown.runtime = &runtime;
        shown.departed = true;
        return shown;
    }
};

/** The Dreamcast look loaded from a synthetic extraction for `party`, with the device it uploads
 * to, Dark Legacy's art it may borrow, which lends the legend items' pictures and the pictures
 * of `lend`, and the words its boxes write. */
struct LoadedLook {
    Fixture files;
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    LentLegends lent{art};
    StringTable strings;
    LegendsDreamcastHudPainter look;

    LoadedLook(std::string_view name, std::span<const HudMember> party,
               std::span<const test::DarkLegacyLookup> lend = {})
        : files(name) {
        for (const test::DarkLegacyLookup& picture : lend) {
            art.give(picture.set, picture.name);
        }
        REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
        REQUIRE(look.load(device, files.root(), art, party));
    }

    /** A view of `seats`, with these words. */
    HudView viewOf(std::array<HudSeat, HudView::kSeats> seats) const {
        HudView view;
        view.seats = std::move(seats);
        view.strings = &strings;
        return view;
    }
    using Slot = void (LegendsDreamcastHudPainter::*)(HudFrame&, const HudView&);
    /** The quads the look's `slot` draws for `view`, on a canvas of its own. */
    std::vector<Quad> drawn(const HudView& view, Slot slot) {
        device.draws.clear();
        Canvas canvas;
        canvas.begin(device, Mat4{1.0f});
        HudFrame frame{canvas, device, art};
        (look.*slot)(frame, view);
        // The Dreamcast's frame is the canvas's own: nothing is drawn through another.
        CHECK(canvas.localTransform() == Mat4{1.0f});
        canvas.end();
        return quadsOf(device);
    }
    /** The quads the look's boxes draw for `view`. */
    std::vector<Quad> boxes(const HudView& view) {
        return drawn(view, &LegendsDreamcastHudPainter::drawStatus);
    }
    /** Readies the look for a frame of `view`, as the scene does before its draws. */
    void prepare(const HudView& view) {
        Canvas canvas;
        canvas.begin(device, Mat4{1.0f});
        HudFrame frame{canvas, device, art};
        look.prepare(frame, view);
        canvas.end();
    }
};

/** The lookups of SELECT's pictures among `art`'s, in turn: the legend items'. */
std::vector<test::DarkLegacyLookup> selectLookups(const test::FakeDarkLegacyArt& art) {
    std::vector<test::DarkLegacyLookup> found;
    for (const test::DarkLegacyLookup& lookup : art.lookups) {
        if (lookup.set == test::DarkLegacyLookup::Set::Select) {
            found.push_back(lookup);
        }
    }
    return found;
}

/** The play clock at the middle of the look's step `step`. */
f32 secondsAt(s32 step) {
    return (static_cast<f32>(step) + 0.5f) / LegendsHudClock::kStepsPerSecond;
}

/** `view` at the look's step `step`, readied for its draws. */
void prepareAt(LoadedLook& dreamcast, HudView& view, s32 step) {
    view.seconds = secondsAt(step);
    dreamcast.prepare(view);
}

/** A party of a yellow warrior and a blue wizard in the first two seats. */
constexpr std::array<HudMember, 2> kWarriorAndWizard{{
    {.seat = 0, .classIndex = kWarriorClass, .color = 0},
    {.seat = 1, .classIndex = kWizardClass, .color = 1},
}};

/** The quads standing in `seat`. */
std::vector<Quad> quadsIn(std::span<const Quad> quads, usize seat) {
    std::vector<Quad> found;
    for (const Quad& quad : quads) {
        if (seatOf(quad) == seat) {
            found.push_back(quad);
        }
    }
    return found;
}

/** A font as TextPainter measures it: one of the fixture's, or a font of no glyphs `rows` tall,
 * which has only its line. */
struct MeasuredFont {
    BitmapFont glyphs;
    test::FakeTexture sheet{16, 16};
    TextPainter text;

    explicit MeasuredFont(const test::DreamcastFont& font) {
        std::vector<BitmapGlyph> cells;
        for (usize index = 0; index < font.codes.size(); ++index) {
            cells.push_back({.code = font.codes[index],
                             .width = font.columns,
                             .x = static_cast<s32>(index) * font.columns,
                             .y = 0});
        }
        glyphs = BitmapFont::fromGlyphs(font.rows, font.space, std::move(cells));
        text.setFont(&glyphs, &sheet);
    }
    explicit MeasuredFont(s32 rows) {
        glyphs = BitmapFont::fromGlyphs(rows, rows / 2, {});
        text.setFont(&glyphs, &sheet);
    }
    ~MeasuredFont() = default;
    GDL_NON_COPYABLE_NON_MOVABLE(MeasuredFont);
};

/** A standing player in seat `seat`, of class `character` in costume colour `costume`, whose
 * save carries `carried`: its runtime, and Dark Legacy's selector over its box, which the case
 * slides up with its own copy of what is carried. */
struct Holder {
    PlayerRuntime runtime;
    PowerupSelector selector;
    Inventory inventory;
    s32 classIndex = 0;
    s32 color = 0;

    Holder(const Inventory& carried, s32 seat, s32 character, s32 costume)
        : inventory(carried), classIndex(character), color(costume) {
        CharacterSave save;
        save.name = "A";
        save.character = character;
        save.color = costume;
        save.progress().inventory = carried;
        runtime.actor.spawn(seat, save, nullptr, Vec3{0}, 0);
    }
    /** Slides the selector up and open, naming the last powerup carried. */
    void open() {
        selector.step(SelectorInput{.up = true}, inventory,
                      PowerupSelector::kSlide / PowerupSelector::kSlidePerTick);
        selector.step({}, inventory, 1);
        REQUIRE(selector.showing());
    }
    /** The seat as the view shows it. */
    HudSeat shown() const {
        HudSeat seat = standing(classIndex, color);
        seat.runtime = &runtime;
        seat.selector = &selector;
        return seat;
    }
};

/** A boss's meter art: two strips, each a background and a fill `width` by `height`. */
TextureSet meterArt(u32 width, u32 height) {
    std::vector<TextureSetEntry> entries;
    std::vector<Image> images;
    for (const char* name : {"METER_BG1", "METER_FG1", "METER_BG2", "METER_FG2"}) {
        entries.push_back({.name = name, .width = width, .height = height, .file = {}});
        images.push_back(Image::filled(width, height, Color::white()));
    }
    return TextureSet::fromDecoded(std::move(entries), std::move(images));
}

/** A boss's meter of `pieces` strips with a background each and these insets, at `health` of
 * `maxHealth`. */
HealthMeterReading meterReading(s32 pieces, s32 leftInset, s32 rightInset, f32 health,
                                f32 maxHealth) {
    HealthMeterReading reading;
    reading.definition.pieces = pieces;
    reading.definition.advance = BossMeter::kPieceWidth;
    reading.definition.leftInset = leftInset;
    reading.definition.rightInset = rightInset;
    reading.definition.shown = true;
    reading.definition.backed = true;
    reading.health = health;
    reading.maxHealth = maxHealth;
    return reading;
}

/** The meters of `readings` drawn from `art`, as the scene binds them, and told to `view`. */
void showMeters(HudView& view, BossMeters& meters, std::span<const HealthMeterReading> readings,
                TextureSet& art) {
    meters.bind(readings, &art);
    view.bossMeters = &meters;
    view.bossCount = hudBossReadingsOf(readings, view.bossReadings);
}

/** The picture `name` of `art`, uploaded to `device` as a meter's draw uploads it. */
const Texture* meterPicture(TextureSet& art, test::FakeRenderDevice& device,
                            std::string_view name) {
    const auto index = art.find(name);
    REQUIRE(index.has_value());
    return &art.texture(device, *index);
}

/** Dark Legacy's hourglass art as plain pictures: the glass, its sand and two frames of
 * falling grains, which SAND_ANIM cycles. */
ItemArchive darkLegacyHourglass() {
    std::vector<TextureSetEntry> entries{
        {.name = "TIMER", .width = 128, .height = 128, .file = {}},
        {.name = "TIMER_SAND", .width = 128, .height = 128, .file = {}},
        {.name = "SAND_ANIM00", .width = 8, .height = 32, .file = {}},
        {.name = "SAND_ANIM01", .width = 8, .height = 32, .file = {}}};
    std::vector<Image> images;
    images.reserve(entries.size());
    for (const TextureSetEntry& entry : entries) {
        images.push_back(Image::filled(entry.width, entry.height, Color::white()));
    }
    ItemArchive archive;
    archive.textures = TextureSet::fromDecoded(std::move(entries), std::move(images));
    formats::AnimationFile animations;
    animations.textureAnimations.push_back(
        {.name = "SAND_ANIM",
         .frameName = {},
         .texture = 2,
         .source = 2,
         .frames = 2,
         .rate = 1,
         .flag = static_cast<s16>(TextureAnimationInfo::kFreeRunning)});
    archive.trees = AnimationSet::fromDecoded(std::move(animations));
    return archive;
}

} // namespace

TEST_CASE("the Dreamcast look keeps the companion arrows above its boxes",
          "[legends][dreamcast-hud]") {
    LegendsDreamcastHudPainter look;
    CHECK(look.id() == kLegendsDreamcastHudStyle);
    CHECK(look.id() != kDarkLegacyHudStyle);
    // The top of a kind-0 box's strip, where Dark Legacy's bar stands too.
    CHECK(look.statusTop() == 304);
    CHECK(look.statusTop() == static_cast<f32>(StatusBoxPainter::kBarY));

    // A look that never loaded draws nothing.
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    const StringTable strings;
    HudView view;
    view.seats[0] = standing(kWarriorClass, 0, "ROWENA");
    view.seats[1] = fallen(standing(kWizardClass, 1), true);
    view.strings = &strings;
    Canvas canvas;
    canvas.begin(device, Mat4{1.0f});
    HudFrame frame{canvas, device, art};
    look.drawStatus(frame, view);
    canvas.end();
    CHECK(device.draws.empty());
}

TEST_CASE("a Dreamcast box draws strip, panel, banner, labels and numbers where the original "
          "does, its counts in the seat's colour",
          "[legends][dreamcast-hud]") {
    LoadedLook dreamcast("dreamcast-hud-box", kWarriorAndWizard);
    // Seat 0, a yellow warrior, carries three keys and two potions, the blue one thrown next,
    // its meter at 45; seat 1, a blue wizard, has little gold and health, its meter empty.
    HudSeat warrior = standing(kWarriorClass, 0);
    warrior.box.keys = 3;
    warrior.box.potions = 2;
    warrior.box.potionKind = 2;
    warrior.turbo = 45.0f;
    HudSeat wizard = standing(kWizardClass, 1, "B", 87);
    wizard.box.level = 3;
    wizard.box.gold = 50;
    const std::vector<Quad> quads = dreamcast.boxes(dreamcast.viewOf({warrior, wizard, {}, {}}));

    // Each box stacks its blits as FUN_8c0ba060 and FUN_8c0c08a0 make them: the strip, the
    // costume's panel, the class's banner, the labels, what is carried, then the turbo arch.
    CHECK(pictureOrder(quads, 0) == Names{"BK_RUNE_STONE_02", "BTMBK_YELO", "WAR_NAME",
                                          "BTMBK_LEVL", "BTMBK_GOLD", "BTMBK_HELTH", "KEY_ICON",
                                          "POTION_ICON_BLU", "TRBO_GRADNT_YELO"});
    CHECK(pictureOrder(quads, 1) == Names{"BK_RUNE_STONE_02", "BTMBK_BLUE", "WIZ_NAME",
                                          "BTMBK_LEVL", "BTMBK_GOLD", "BTMBK_HELTH"});
    // The panels and the banners come from each seat's own costume bank.
    CHECK(folderOf(quads, "BTMBK_YELO", 0) == "players/war/yel");
    CHECK(folderOf(quads, "WAR_NAME", 0) == "players/war/yel");
    CHECK(folderOf(quads, "BTMBK_BLUE", 1) == "players/wiz/blu");
    CHECK(folderOf(quads, "WIZ_NAME", 1) == "players/wiz/blu");

    // The art stands from 8 left of the column: the strip at 306, the panel's top half at 320,
    // the banner at 338; the labels at (X+34, 323), (X+5, 357) and (X+53, 357).
    const auto only = [&](std::string_view name, usize seat) {
        const std::vector<Quad> found = picturesIn(quads, name, seat);
        REQUIRE(found.size() == 1);
        return found.front();
    };
    for (const usize seat : {usize{0}, usize{1}}) {
        CAPTURE(seat);
        const auto x = static_cast<f32>(dreamcastSeatX(seat));
        checkRect(only("BK_RUNE_STONE_02", seat).area, {x - 8, 306, 128, 16});
        const Quad panel = only(seat == 0 ? "BTMBK_YELO" : "BTMBK_BLUE", seat);
        checkRect(panel.area, {x - 8, 320, 128, 64});
        checkRect(panel.uv, {0, 0, 1, 0.5f});
        checkRect(only(seat == 0 ? "WAR_NAME" : "WIZ_NAME", seat).area, {x - 8, 338, 128, 16});
        checkRect(only("BTMBK_LEVL", seat).area, {x + 34, 323, 32, 8});
        checkRect(only("BTMBK_GOLD", seat).area, {x + 5, 357, 32, 8});
        checkRect(only("BTMBK_HELTH", seat).area, {x + 53, 357, 64, 8});
    }
    // KEY_ICON at (X+1, 323) and the next potion's icon at (X+68, 323).
    checkRect(only("KEY_ICON", 0).area, {9, 323, 16, 16});
    checkRect(only("POTION_ICON_BLU", 0).area, {76, 323, 16, 16});
    for (const Quad& quad : quads) {
        if (!isText(quad)) {
            CHECK(quad.color == Color::white());
        }
    }

    // The level in font 1, white, centred on the box at 330. Gold and health in font 4 at 365,
    // gold from X+3 or, narrower than 32, ending at X+35, health ending at X+92; the key and
    // potion counts from X+18 and X+82 at 323; all of them in the seat's ink (8c14bb98).
    CHECK(kYellowInk == Color::rgba(0xFF, 0xFF, 0x80));
    CHECK(kBlueInk == Color::rgba(0x87, 0xCE, 0xEB));
    CHECK(linesOf(quads, Fixture::kSmallFont) ==
          std::vector<Line>{centredAt(Fixture::kSmallFont, 56, 330, "12", Color::white()),
                            centredAt(Fixture::kSmallFont, 168, 330, "3", Color::white())});
    CHECK(linesOf(quads, Fixture::kScoreFont) ==
          std::vector<Line>{
              lineAt(8 + 3, 365, "1250", kYellowInk), lineAt(8 + 92 - 24, 365, "600", kYellowInk),
              lineAt(8 + 18, 323, "3", kYellowInk), lineAt(8 + 82, 323, "2", kYellowInk),
              lineAt(120 + 35 - 16, 365, "50", kBlueInk),
              lineAt(120 + 92 - 16, 365, "87", kBlueInk)});
    // The gold and health come before what the box carries.
    usize lastNumber = 0;
    usize firstKey = quads.size();
    for (usize index = 0; index < quads.size(); ++index) {
        if (Fixture::sheetOf(quads[index].texture, Fixture::kScoreFont) &&
            quads[index].area.y > 360) {
            lastNumber = seatOf(quads[index]) == 0 ? index : lastNumber;
        }
        if (Fixture::pictureOf(quads[index].texture).name == "KEY_ICON") {
            firstKey = std::min(firstKey, index);
        }
    }
    CHECK(lastNumber < firstKey);

    // A lone player in the third seat stands in that seat's column.
    const std::vector<Quad> lone =
        dreamcast.boxes(dreamcast.viewOf({HudSeat{}, {}, standing(kWarriorClass, 0), {}}));
    REQUIRE_FALSE(lone.empty());
    for (const Quad& quad : lone) {
        CHECK(seatOf(quad) == 2);
    }
    const std::vector<Quad> strip = picturesIn(lone, "BK_RUNE_STONE_02");
    REQUIRE(strip.size() == 1);
    checkRect(strip.front().area, {288, 306, 128, 16});

    // FUN_8c0c0f40 holds the gold to 99999 (8c0c1320) and FUN_8c0c16e0 the health to 9999
    // (8c0c186c).
    HudSeat rich = standing(kWarriorClass, 0, "C", 12345);
    rich.box.gold = 123456;
    CHECK(linesOf(dreamcast.boxes(dreamcast.viewOf({rich, {}, {}, {}})), Fixture::kScoreFont) ==
          std::vector<Line>{lineAt(8 + 3, 365, "99999", kYellowInk),
                            lineAt(8 + 92 - 32, 365, "9999", kYellowInk)});
}

TEST_CASE("keys and potions show only while carried", "[legends][dreamcast-hud]") {
    const std::array<HudMember, 3> party{{
        {.seat = 0, .classIndex = kWarriorClass, .color = 0},
        {.seat = 1, .classIndex = kWizardClass, .color = 1},
        {.seat = 2, .classIndex = kWarriorClass, .color = 2},
    }};
    LoadedLook dreamcast("dreamcast-hud-carried", party);
    // Seat 0 carries nothing; seat 1 a key alone; seat 2 a green potion alone.
    const HudSeat nothing = standing(kWarriorClass, 0);
    HudSeat keyed = standing(kWizardClass, 1);
    keyed.box.keys = 1;
    HudSeat dosed = standing(kWarriorClass, 2);
    dosed.box.potions = 1;
    dosed.box.potionKind = 4;
    const std::vector<Quad> quads = dreamcast.boxes(dreamcast.viewOf({nothing, keyed, dosed, {}}));
    // FUN_8c0c16e0 shows the key and the potion, and their counts, only while some are carried.
    CHECK(picturesIn(quads, "KEY_ICON", 0).empty());
    CHECK(picturesIn(quads, "KEY_ICON", 1).size() == 1);
    CHECK(picturesIn(quads, "KEY_ICON", 2).empty());
    for (const std::string_view potion :
         {"POTION_ICON_RED", "POTION_ICON_BLU", "POTION_ICON_YEL"}) {
        CAPTURE(potion);
        CHECK(picturesIn(quads, potion).empty());
    }
    const std::vector<Quad> green = picturesIn(quads, "POTION_ICON_GRE");
    REQUIRE(green.size() == 1);
    CHECK(seatOf(green.front()) == 2);
    checkRect(green.front().area, {296 + 68, 323, 16, 16});
    // Only the key's count and the potion's are written in their row.
    std::vector<Line> counts;
    for (const Line& line : linesOf(quads, Fixture::kScoreFont)) {
        if (line.y == 323.5f) {
            counts.push_back(line);
        }
    }
    CHECK(counts == std::vector<Line>{lineAt(120 + 18, 323, "1", kBlueInk),
                                      lineAt(296 + 82, 323, "1", kRedInk)});
}

TEST_CASE("a class with no Dreamcast banner shows Dark Legacy's name in its place, and Sumner "
          "the N64 HUD's word",
          "[legends][dreamcast-hud]") {
    const std::array<HudMember, 4> party{{
        {.seat = 0, .classIndex = kOgreClass, .color = 0},
        {.seat = 1, .classIndex = kSumnerClass, .color = 1},
        {.seat = 2, .classIndex = kWarriorClass, .color = 2},
        {.seat = 3, .classIndex = kUnicornClass, .color = 3},
    }};
    LoadedLook dreamcast("dreamcast-hud-bannerless", party);
    const std::vector<Quad> quads =
        dreamcast.boxes(dreamcast.viewOf({standing(kOgreClass, 0), standing(kSumnerClass, 1),
                                          standing(kWarriorClass, 2), standing(kUnicornClass, 3)}));
    // The Dreamcast has no ogre, Sumner or unicorn. Each takes the warrior's panel of its colour,
    // which every class's of that colour is, and in the banner's place Dark Legacy's lower-case
    // name of its class, or for Sumner the N64 HUD's word, in font32 at half its size, white,
    // centred on the box at 338.
    CHECK(linesOf(quads, Fixture::kLargeFont, kHalf) ==
          std::vector<Line>{
              centredAt(Fixture::kLargeFont, 56, 338, "Ogre", Color::white(), kHalf),
              centredAt(Fixture::kLargeFont, 168, 338, "Sumner", Color::white(), kHalf),
              centredAt(Fixture::kLargeFont, 456, 338, "Unicorn", Color::white(), kHalf)});
    CHECK(pictureOrder(quads, 0) ==
          Names{"BK_RUNE_STONE_02", "BTMBK_YELO", "BTMBK_LEVL", "BTMBK_GOLD", "BTMBK_HELTH"});
    CHECK(folderOf(quads, "BTMBK_YELO", 0) == "players/war/yel");
    CHECK(folderOf(quads, "BTMBK_BLUE", 1) == "players/war/blu");
    CHECK(folderOf(quads, "BTMBK_GREEN", 3) == "players/war/gre");
    // The warrior keeps its own banner.
    CHECK(folderOf(quads, "WAR_NAME", 2) == "players/war/red");
    for (const usize seat : {usize{0}, usize{1}, usize{3}}) {
        CAPTURE(seat);
        CHECK(picturesIn(quads, "WAR_NAME", seat).empty());
    }
}

TEST_CASE("an empty seat draws nothing; a departed seat keeps the kind-0 panel",
          "[legends][dreamcast-hud]") {
    LoadedLook dreamcast("dreamcast-hud-departed", kWarriorAndWizard);
    const Departed gone(1, kWizardClass, 2, "MERLIN");
    const std::vector<Quad> quads =
        dreamcast.boxes(dreamcast.viewOf({HudSeat{}, gone.seat(), {}, {}}));
    // FUN_8c0bbee0 rebuilds a quitting player's box as kind 0, in its costume's colour, and the
    // box is never hidden after: the strip at 304 and the panel's top half at 320, and no text.
    CHECK(pictureOrder(quads, 1) == Names{"S3_WRED", "S4_WRED"});
    REQUIRE(quads.size() == 2);
    checkRect(quads[0].area, {112, 304, 128, 16});
    checkRect(quads[1].area, {112, 320, 128, 64});
    checkRect(quads[1].uv, {0, 0, 1, 0.5f});
    // The seats nobody joined draw nothing: FUN_8c0ba060 makes all their blits hidden.
    for (const Quad& quad : quads) {
        CHECK(seatOf(quad) == 1);
    }
    CHECK(dreamcast.boxes(dreamcast.viewOf({HudSeat{}, {}, {}, {}})).empty());
}

TEST_CASE("a fallen player's Dreamcast box says IN TOWER on the kind-0 panel",
          "[legends][dreamcast-hud]") {
    const std::array<HudMember, 3> party{{
        {.seat = 0, .classIndex = kWarriorClass, .color = 1},
        {.seat = 1, .classIndex = kWizardClass, .color = 3},
        {.seat = 2, .classIndex = kWarriorClass, .color = 2},
    }};
    LoadedLook dreamcast("dreamcast-hud-fallen", party);
    // Seat 0, blue, waits in the tower; seat 1, green, is asked whether to wait there; seat 2,
    // red, is still dying.
    const HudSeat waiting = fallen(standing(kWarriorClass, 1), true);
    const HudSeat asked = fallen(standing(kWizardClass, 3), true, true);
    const HudSeat dying = fallen(standing(kWarriorClass, 2), false);
    const std::vector<Quad> quads = dreamcast.boxes(dreamcast.viewOf({waiting, asked, dying, {}}));

    // FUN_8c0bbf80 and FUN_8c0bc040 rebuild a fallen player's box as kind 0: the coloured strip
    // at 304 and panel at 320, with no banner, labels, numbers, keys or arch.
    CHECK(pictureOrder(quads, 0) == Names{"S3_WBLUE", "S4_WBLUE"});
    CHECK(pictureOrder(quads, 1) == Names{"S3_WGREEN", "S4_WGREEN"});
    const std::vector<Quad> strip = picturesIn(quads, "S3_WBLUE");
    const std::vector<Quad> panel = picturesIn(quads, "S4_WBLUE");
    REQUIRE(strip.size() == 1);
    REQUIRE(panel.size() == 1);
    checkRect(strip.front().area, {0, 304, 128, 16});
    checkRect(panel.front().area, {0, 320, 128, 64});
    checkRect(panel.front().uv, {0, 0, 1, 0.5f});
    // FUN_8c0c16e0 writes IN TOWER (8c1256b4) in font 1, in the seat's ink, centred on the box
    // at 344, for the player waiting in the tower; the one asked whether to wait there is asked
    // instead (FUN_8c0bda40). The dying player's level follows.
    CHECK(
        linesOf(quads, Fixture::kSmallFont) ==
        std::vector<Line>{centredAt(Fixture::kSmallFont, 56, 344, "IN TOWER", kBlueInk),
                          centredAt(Fixture::kSmallFont, 168, 336, "WAIT", kGreenInk),
                          centredAt(Fixture::kSmallFont, 168, 344, "IN TOWER?", kGreenInk),
                          centredAt(Fixture::kSmallFont, 168, 356, "YES", Layout::kPromptChosenInk),
                          centredAt(Fixture::kSmallFont, 168, 368, "NO", kGreenInk),
                          centredAt(Fixture::kSmallFont, 344, 330, "12", Color::white())});

    // Dying, the box is still the panel with its banner, labels, level, gold and health, and no
    // arch: FUN_8c0bbe40 keeps the panel for the death (state 11), and FUN_8c0c0f40 writes the
    // gold in it, leaving it out only in states 5 and 6, which follow state 3.
    CHECK(pictureOrder(quads, 2) == Names{"BK_RUNE_STONE_02", "BTMBK_RED", "WAR_NAME", "BTMBK_LEVL",
                                          "BTMBK_GOLD", "BTMBK_HELTH"});
    CHECK(linesOf(quads, Fixture::kScoreFont) ==
          std::vector<Line>{lineAt(296 + 3, 365, "1250", kRedInk),
                            lineAt(296 + 92 - 8, 365, "0", kRedInk)});
    CHECK(linesOf(quads, Fixture::kLargeFont, kHalf).empty());
}

TEST_CASE("a missing or truncated class bank falls back to another class's panel",
          "[legends][dreamcast-hud]") {
    const std::array<HudMember, 2> party{{
        {.seat = 0, .classIndex = kDwarfClass, .color = 2},
        {.seat = 1, .classIndex = kWizardClass, .color = 1},
    }};
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    /** What a red dwarf's box shows beside a blue wizard's: where its panel came from, whether
     * its banner stands, and the names written in the banners' place. */
    struct Shown {
        std::string_view panel;
        bool banner = false;
        std::vector<Line> names;
    };
    const auto shownBy = [&](LegendsDreamcastHudPainter& look) {
        test::FakeRenderDevice drawing;
        Canvas canvas;
        canvas.begin(drawing, Mat4{1.0f});
        HudFrame frame{canvas, drawing, art};
        HudView view;
        view.seats[0] = standing(kDwarfClass, 2);
        view.seats[1] = standing(kWizardClass, 1);
        view.strings = &strings;
        look.drawStatus(frame, view);
        canvas.end();
        const std::vector<Quad> quads = quadsOf(drawing);
        // The boxes stand either way, and the wizard's is its own.
        CHECK(picturesIn(quads, "BK_RUNE_STONE_02").size() == 2);
        CHECK(folderOf(quads, "BTMBK_BLUE", 1) == "players/wiz/blu");
        CHECK(folderOf(quads, "WIZ_NAME", 1) == "players/wiz/blu");
        return Shown{.panel = folderOf(quads, "BTMBK_RED", 0),
                     .banner = !picturesIn(quads, "DWF_NAME", 0).empty(),
                     .names = linesOf(quads, Fixture::kLargeFont, kHalf)};
    };
    const std::vector<Line> dwarf{
        centredAt(Fixture::kLargeFont, 56, 338, "Dwarf", Color::white(), kHalf)};

    SECTION("everything there") {
        const Fixture files("dreamcast-hud-class-whole");
        LegendsDreamcastHudPainter look;
        CHECK(look.load(device, files.root(), art, party));
        const Shown shown = shownBy(look);
        CHECK(shown.panel == "players/dwf/red");
        CHECK(shown.banner);
        CHECK(shown.names.empty());
    }
    SECTION("a class bank missing") {
        const Fixture files("dreamcast-hud-class-missing");
        REQUIRE(std::filesystem::remove(files.root() / "players/dwf/red/objects.rom"));
        LegendsDreamcastHudPainter look;
        bool loaded = false;
        const auto run =
            test::logWhile([&] { loaded = look.load(device, files.root(), art, party); });
        REQUIRE_FALSE(run.failure);
        CHECK(loaded);
        CHECK(run.warnings.empty());
        // The warrior's panel of its colour stands in, and the class is named in the banner's
        // place.
        const Shown shown = shownBy(look);
        CHECK(shown.panel == "players/war/red");
        CHECK_FALSE(shown.banner);
        CHECK(shown.names == dwarf);
    }
    SECTION("a class bank cut short") {
        const Fixture files("dreamcast-hud-class-short");
        files.truncate("players/dwf/red/objects.rom");
        LegendsDreamcastHudPainter look;
        bool loaded = false;
        const auto run =
            test::logWhile([&] { loaded = look.load(device, files.root(), art, party); });
        REQUIRE_FALSE(run.failure);
        CHECK(loaded);
        REQUIRE(run.warnings.size() == 1);
        CHECK(run.warnings.front().find("players/dwf/red") != std::string::npos);
        const Shown shown = shownBy(look);
        CHECK(shown.panel == "players/war/red");
        CHECK_FALSE(shown.banner);
        CHECK(shown.names == dwarf);
    }
    SECTION("the warrior's bank gone too") {
        const Fixture files("dreamcast-hud-class-gone");
        REQUIRE(std::filesystem::remove(files.root() / "players/dwf/red/objects.rom"));
        REQUIRE(std::filesystem::remove(files.root() / "players/war/red/textures.rom"));
        LegendsDreamcastHudPainter look;
        bool loaded = false;
        const auto run =
            test::logWhile([&] { loaded = look.load(device, files.root(), art, party); });
        REQUIRE_FALSE(run.failure);
        CHECK(loaded);
        CHECK(run.warnings.empty());
        // The box stands without a panel, its class still named.
        const Shown shown = shownBy(look);
        CHECK(shown.panel.empty());
        CHECK_FALSE(shown.banner);
        CHECK(shown.names == dwarf);
    }
}

TEST_CASE("the typed name is never drawn", "[legends][dreamcast-hud]") {
    const std::array<HudMember, 4> party{{
        {.seat = 0, .classIndex = kWarriorClass, .color = 0},
        {.seat = 1, .classIndex = kWizardClass, .color = 1},
        {.seat = 2, .classIndex = kWarriorClass, .color = 2},
        {.seat = 3, .classIndex = kOgreClass, .color = 3},
    }};
    LoadedLook dreamcast("dreamcast-hud-typed-names", party);
    // The names are spelt in the fonts' own letters, so that they would be read back were they
    // written.
    const std::array<std::string, 4> typed{"ROWENA", "MERLIN", "THYRA", "GROG"};
    HudSeat standingWarrior = standing(kWarriorClass, 0, typed[0]);
    standingWarrior.turbo = 100.0f;
    const std::vector<Quad> quads = dreamcast.boxes(dreamcast.viewOf(
        {standingWarrior, fallen(standing(kWizardClass, 1, typed[1]), true),
         fallen(standing(kWarriorClass, 2, typed[2]), false), standing(kOgreClass, 3, typed[3])}));
    REQUIRE_FALSE(quads.empty());
    std::vector<Line> written = linesOf(quads, Fixture::kSmallFont);
    for (const Line& line : linesOf(quads, Fixture::kScoreFont)) {
        written.push_back(line);
    }
    for (const Line& line : linesOf(quads, Fixture::kLargeFont, kHalf)) {
        written.push_back(line);
    }
    REQUIRE_FALSE(written.empty());
    for (const Line& line : written) {
        for (const std::string& name : typed) {
            CAPTURE(line.text, name);
            CHECK(toLowerAscii(line.text).find(toLowerAscii(name)) == std::string::npos);
        }
    }
    // Nor is any of it in Dark Legacy's small capitals, as its own boxes write it.
    for (const Quad& quad : quads) {
        CHECK(quad.texture != &dreamcast.art.smallCapsSheet());
    }
}

TEST_CASE("the turbo arch stands over a standing player's box, cropped from its centre, and a "
          "full one glows",
          "[legends][dreamcast-hud]") {
    LoadedLook dreamcast("dreamcast-hud-turbo", kWarriorAndWizard);
    HudSeat rising = standing(kWarriorClass, 0);
    rising.turbo = 45.0f;
    HudSeat full = standing(kWizardClass, 1);
    full.turbo = 100.0f;
    HudView view = dreamcast.viewOf({rising, full, {}, {}});
    prepareAt(dreamcast, view, 0);
    const std::vector<Quad> quads = dreamcast.boxes(view);

    // Seat 0's yellow arch, cropped about texel 56 to a half-width of 31 (FUN_8c0c13e0).
    const std::vector<Quad> arch = picturesIn(quads, "TRBO_GRADNT_YELO", 0);
    REQUIRE(arch.size() == 1);
    checkRect(arch.front().area, {8 - 8 + 57 - 31, 306, 62, 32});
    checkRect(arch.front().uv, {25.0f / 128, 0, 62.0f / 128, 1});
    CHECK(picturesIn(quads, "TURBO_GLOW", 0).empty());
    // Seat 1's is whole, its glow over it.
    CHECK(pictureOrder(quads, 1) == Names{"BK_RUNE_STONE_02", "BTMBK_BLUE", "WIZ_NAME",
                                          "BTMBK_LEVL", "BTMBK_GOLD", "BTMBK_HELTH", "TRBO_FULL",
                                          "TURBO_GLOW"});
    const std::vector<Quad> whole = picturesIn(quads, "TRBO_FULL", 1);
    REQUIRE(whole.size() == 1);
    checkRect(whole.front().area, {112, 306, 128, 32});
    checkRect(whole.front().uv, {0, 0, 1, 1});

    // The glow pulses a step at a time while the arch is full, its transparency 0, 12, 24, ...;
    // while the meter is not full it is hidden and its pulse waits, and a rewind starts it over.
    const auto glowAt = [&](s32 step) -> std::optional<u8> {
        prepareAt(dreamcast, view, step);
        const std::vector<Quad> glow = picturesIn(dreamcast.boxes(view), "TURBO_GLOW", 1);
        if (glow.empty()) {
            return std::nullopt;
        }
        REQUIRE(glow.size() == 1);
        checkRect(glow.front().area, {112, 306, 128, 32});
        return glow.front().color.a;
    };
    const auto opaqueAt = [](s32 transparency) {
        return std::optional<u8>{static_cast<u8>(255 - transparency)};
    };
    CHECK(glowAt(0) == opaqueAt(0));
    CHECK(glowAt(1) == opaqueAt(12));
    CHECK(glowAt(2) == opaqueAt(24));
    view.seats[1].turbo = 50.0f;
    for (s32 step = 3; step < 6; ++step) {
        CAPTURE(step);
        CHECK_FALSE(glowAt(step));
    }
    view.seats[1].turbo = 100.0f;
    CHECK(glowAt(6) == opaqueAt(36));
    CHECK(glowAt(2) == opaqueAt(0));
    CHECK(glowAt(3) == opaqueAt(12));
}

TEST_CASE("runestones line a Dreamcast box, and the bosses' eight keys show along it while the "
          "relics are shown",
          "[legends][dreamcast-hud]") {
    LoadedLook dreamcast("dreamcast-hud-relics", kWarriorAndWizard);
    // Seat 1, a blue wizard, holds runes 1, 5 and 12 and every boss's key; seat 0 holds none.
    HudSeat wizard = standing(kWizardClass, 1);
    wizard.box.runes = 0x0811;
    wizard.box.bossKeys = 0xFF;
    HudView view = dreamcast.viewOf({standing(kWarriorClass, 0), wizard, {}, {}});
    view.relicsShown = true;
    const std::vector<Quad> shown = dreamcast.boxes(view);

    // FUN_8c0ba060: the runes, SM_RUNE_<colour>_0<n> of the static set, along the strip from X-1
    // at 306, 8 a rune and a unit more after each colour's three: blue, red, yellow, green.
    const auto only = [](std::span<const Quad> quads, std::string_view name) {
        const std::vector<Quad> found = picturesIn(quads, name, 1);
        REQUIRE(found.size() == 1);
        CHECK(Fixture::pictureOf(found.front().texture).folder == Fixture::kStaticFolder);
        return found.front().area;
    };
    checkRect(only(shown, "SM_RUNE_BLU_01"), {119, 306, 8, 8});
    checkRect(only(shown, "SM_RUNE_RED_02"), {119 + 32 + 1, 306, 8, 8});
    checkRect(only(shown, "SM_RUNE_GRE_03"), {119 + 88 + 3, 306, 8, 8});
    // The bosses' eight keys in the Dreamcast's lightning art, from X+8 at 300 as its own four
    // stand, but at Dark Legacy's twelve apart, in the GameCube's colours (0x801205F8).
    CHECK(StatusBoxPainter::kKeyColours ==
          std::array<std::string_view, 8>{"BLU", "RED", "YEL", "GRE", "GRE", "RED", "YEL", "BLU"});
    std::vector<Quad> keys;
    for (const Quad& quad : shown) {
        if (Fixture::pictureOf(quad.texture).name.starts_with("SM_KEY_")) {
            keys.push_back(quad);
        }
    }
    REQUIRE(keys.size() == 8);
    for (usize key = 0; key < keys.size(); ++key) {
        CAPTURE(key);
        CHECK(Fixture::pictureOf(keys[key].texture).name ==
              std::format("SM_KEY_{}", StatusBoxPainter::kKeyColours.at(key)));
        checkRect(keys[key].area, {static_cast<f32>(120 + 8 + (12 * key)), 300, 16, 16});
    }
    // The rows stand over the rest of the box, the runes before the keys, as FUN_8c0ba060 makes
    // them.
    const Names panel{"BK_RUNE_STONE_02", "BTMBK_BLUE", "WIZ_NAME",
                      "BTMBK_LEVL",       "BTMBK_GOLD", "BTMBK_HELTH"};
    Names withRelics = panel;
    for (const std::string_view name :
         {"SM_RUNE_BLU_01", "SM_RUNE_RED_02", "SM_RUNE_GRE_03", "SM_KEY_BLU", "SM_KEY_RED",
          "SM_KEY_YEL", "SM_KEY_GRE", "SM_KEY_GRE", "SM_KEY_RED", "SM_KEY_YEL", "SM_KEY_BLU"}) {
        withRelics.push_back(name);
    }
    CHECK(pictureOrder(shown, 1) == withRelics);
    CHECK(pictureOrder(shown, 0) == Names{"BK_RUNE_STONE_02", "BTMBK_YELO", "WAR_NAME",
                                          "BTMBK_LEVL", "BTMBK_GOLD", "BTMBK_HELTH"});

    // While the relics are not shown, FUN_8c0c2120 hides the keys, but never the runes, which
    // the box keeps from when it was made.
    view.relicsShown = false;
    Names runesAlone = panel;
    for (const std::string_view name : {"SM_RUNE_BLU_01", "SM_RUNE_RED_02", "SM_RUNE_GRE_03"}) {
        runesAlone.push_back(name);
    }
    CHECK(pictureOrder(dreamcast.boxes(view), 1) == runesAlone);
    // None of it is borrowed: the runestones and keys are the Dreamcast's own art, never asked of
    // Dark Legacy's.
    REQUIRE_FALSE(dreamcast.art.lookups.empty());
    for (const test::DarkLegacyLookup& lookup : dreamcast.art.lookups) {
        CAPTURE(lookup.name);
        CHECK_FALSE(lookup.name.starts_with("SM_RUNE_"));
        CHECK_FALSE(lookup.name.starts_with("SM_KEY_"));
        CHECK(lookup.name != "RUNE13");
    }
}

TEST_CASE("the thirteenth rune sits at Dark Legacy's place in the Dreamcast box",
          "[legends][dreamcast-hud]") {
    const std::array<HudMember, 1> party{{{.seat = 2, .classIndex = kWarriorClass, .color = 2}}};
    LoadedLook dreamcast("dreamcast-hud-rune13", party);
    // Seat 2, a red warrior, holds the thirteenth rune beside the first, and the second boss's
    // key.
    HudSeat warrior = standing(kWarriorClass, 2);
    warrior.box.runes = static_cast<u16>(StatusBoxPainter::kThirteenthRune | 0x1);
    warrior.box.bossKeys = 0x2;
    HudView view = dreamcast.viewOf({HudSeat{}, {}, warrior, {}});
    // Dark Legacy's box shows it 8 in at 340 (hod_blit); the Dreamcast's box stands from 8 left
    // of its column, so its own RUNE13, which no Dreamcast box shows, stands at the column.
    CHECK(StatusBoxPainter::kRune13X == 8);
    CHECK(StatusBoxPainter::kRune13Y == 340);
    for (const bool relicsShown : {false, true}) {
        CAPTURE(relicsShown);
        view.relicsShown = relicsShown;
        const std::vector<Quad> quads = dreamcast.boxes(view);
        const std::vector<Quad> rune = picturesIn(quads, "RUNE13", 2);
        REQUIRE(rune.size() == 1);
        checkRect(rune.front().area, {296, 340, 16, 16});
        CHECK(Fixture::pictureOf(rune.front().texture).folder == Fixture::kStaticFolder);
        // It is none of the strip's runes, and it shows whether the relics do or not, after them.
        Names expected{"BK_RUNE_STONE_02", "BTMBK_RED",   "WAR_NAME",      "BTMBK_LEVL",
                       "BTMBK_GOLD",       "BTMBK_HELTH", "SM_RUNE_BLU_01"};
        if (relicsShown) {
            expected.emplace_back("SM_KEY_RED");
        }
        expected.emplace_back("RUNE13");
        CHECK(pictureOrder(quads, 2) == expected);
    }
    // Twelve runes without it show no RUNE13.
    view.seats[2].box.runes = 0x0FFF;
    CHECK(picturesIn(dreamcast.boxes(view), "RUNE13").empty());
}

TEST_CASE("legend items take the Dreamcast's five places, Dark Legacy's own items the free ones",
          "[legends][dreamcast-hud]") {
    LoadedLook dreamcast("dreamcast-hud-legends", kWarriorAndWizard);
    // Seat 0 holds the scimitar (realm 1), the lamp (3), the book (7), the fire scroll (9), the
    // lantern (10) and the javelin (11); seat 1 the ice axe (2), the bellows (4) and the soul
    // savior (5).
    HudSeat warrior = standing(kWarriorClass, 0);
    warrior.legends = legendsOf({1, 3, 7, 9, 10, 11});
    HudSeat wizard = standing(kWizardClass, 1);
    wizard.legends = legendsOf({2, 4, 5});
    const std::vector<Quad> quads = dreamcast.boxes(dreamcast.viewOf({warrior, wizard, {}, {}}));

    // 8c14bbf4 and FUN_8c0c16e0: the Dreamcast's own five, QUEST_ICON1..5 of its powerups bank,
    // stand 16 square in their own places, realm 1's at (X+34, 362), 2's at (X+42, 362), 3's at
    // (X+34, 370), 4's at (X+42, 370) and 5's at (X+38, 367).
    const auto icon = [&](std::string_view name, usize seat) {
        const std::vector<Quad> found = picturesIn(quads, name, seat);
        REQUIRE(found.size() == 1);
        CHECK(Fixture::pictureOf(found.front().texture).folder == Fixture::kPowerupsFolder);
        return found.front().area;
    };
    checkRect(icon("QUEST_ICON1", 0), {8 + 34, 362, 16, 16});
    checkRect(icon("QUEST_ICON3", 0), {8 + 34, 370, 16, 16});
    checkRect(icon("QUEST_ICON2", 1), {120 + 42, 362, 16, 16});
    checkRect(icon("QUEST_ICON4", 1), {120 + 42, 370, 16, 16});
    checkRect(icon("QUEST_ICON5", 1), {120 + 38, 367, 16, 16});
    // Dark Legacy's others take, in realm order, the places whose own item is not held, each in
    // Dark Legacy's SELECT picture fitted into its place about its middle: the book, 32 by 24,
    // the ice axe's place, the fire scroll, 24 by 48, the bellows', and the lantern, 8 square,
    // the soul savior's. The javelin finds no place left.
    const auto lent = [&](std::string_view name) {
        const std::vector<Quad> found = dreamcast.lent.drawn(quads, name);
        REQUIRE(found.size() == 1);
        CHECK(seatOf(found.front()) == 0);
        return found.front().area;
    };
    checkRect(lent("BOOK"), {8 + 42, 362 + 2, 16, 12});
    checkRect(lent("FIRE_SCROLL"), {8 + 42 + 4, 370, 8, 16});
    checkRect(lent("LANTERN"), {8 + 38, 367, 16, 16});
    CHECK(dreamcast.lent.drawn(quads, "JAVILIN").empty());
    // The Dreamcast's own five show its icons, never Dark Legacy's pictures of them.
    for (const std::string_view name : {"SCIMITAR", "ICE_AX", "LAMP", "BILLOWS", "SOUL_SAVIOR"}) {
        CAPTURE(name);
        CHECK(dreamcast.lent.drawn(quads, name).empty());
    }
    // They stand last in the box, in their places' order.
    const Names panel{"BK_RUNE_STONE_02", "BTMBK_YELO", "WAR_NAME",
                      "BTMBK_LEVL",       "BTMBK_GOLD", "BTMBK_HELTH"};
    Names placed = panel;
    for (const std::string_view name :
         {"QUEST_ICON1", "BOOK", "QUEST_ICON3", "FIRE_SCROLL", "LANTERN"}) {
        placed.push_back(name);
    }
    CHECK(boxOrder(quads, 0, dreamcast.lent) == placed);
    CHECK(boxOrder(quads, 1, dreamcast.lent) == Names{"BK_RUNE_STONE_02", "BTMBK_BLUE", "WIZ_NAME",
                                                      "BTMBK_LEVL", "BTMBK_GOLD", "BTMBK_HELTH",
                                                      "QUEST_ICON2", "QUEST_ICON4", "QUEST_ICON5"});
    // Holding nothing shows nothing in them.
    const std::vector<Quad> none =
        dreamcast.boxes(dreamcast.viewOf({standing(kWarriorClass, 0), {}, {}, {}}));
    CHECK(boxOrder(none, 0, dreamcast.lent) == panel);
}

TEST_CASE("a missing or truncated powerups bank gives the legend items Dark Legacy's pictures",
          "[legends][dreamcast-hud]") {
    using Set = test::DarkLegacyLookup::Set;
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    const LentLegends lent(art);
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    const auto asked = [](std::initializer_list<std::string_view> names) {
        std::vector<test::DarkLegacyLookup> lookups;
        for (const std::string_view name : names) {
            lookups.push_back({Set::Select, std::string{name}});
        }
        return lookups;
    };
    /** What a yellow warrior holding the scimitar, the soul savior and the book shows. */
    const auto shownBy = [&](LegendsDreamcastHudPainter& look) {
        test::FakeRenderDevice drawing;
        Canvas canvas;
        canvas.begin(drawing, Mat4{1.0f});
        HudFrame frame{canvas, drawing, art};
        HudView view;
        view.seats[0] = standing(kWarriorClass, 0);
        view.seats[0].legends = legendsOf({1, 5, 7});
        view.strings = &strings;
        look.drawStatus(frame, view);
        canvas.end();
        return quadsOf(drawing);
    };
    /** Checks the scimitar and the soul savior show in Dark Legacy's pictures in their own places,
     * fitted, and the book in the ice axe's. */
    const auto checkStandIns = [&](std::span<const Quad> quads) {
        for (const std::string_view name : {"QUEST_ICON1", "QUEST_ICON5"}) {
            CAPTURE(name);
            CHECK(picturesIn(quads, name).empty());
        }
        const auto area = [&](std::string_view name) {
            const std::vector<Quad> found = lent.drawn(quads, name);
            REQUIRE(found.size() == 1);
            return found.front().area;
        };
        checkRect(area("SCIMITAR"), {8 + 34, 362 + 4, 16, 8});
        checkRect(area("SOUL_SAVIOR"), {8 + 38 + 4, 367, 8, 16});
        checkRect(area("BOOK"), {8 + 42, 362 + 2, 16, 12});
    };

    SECTION("everything there") {
        const Fixture files("dreamcast-hud-powerups-whole");
        LegendsDreamcastHudPainter look;
        CHECK(look.load(device, files.root(), art, kWarriorAndWizard));
        CHECK(selectLookups(art) == asked({"BOOK", "FIRE_SCROLL", "LANTERN", "JAVILIN"}));
        const std::vector<Quad> quads = shownBy(look);
        CHECK(picturesIn(quads, "QUEST_ICON1", 0).size() == 1);
        CHECK(picturesIn(quads, "QUEST_ICON5", 0).size() == 1);
        CHECK(lent.drawn(quads, "SCIMITAR").empty());
        CHECK(lent.drawn(quads, "SOUL_SAVIOR").empty());
    }
    SECTION("the bank missing") {
        const Fixture files("dreamcast-hud-powerups-missing");
        REQUIRE(std::filesystem::remove(files.root() / "powerups/objects.rom"));
        LegendsDreamcastHudPainter look;
        bool loaded = false;
        const auto run = test::logWhile(
            [&] { loaded = look.load(device, files.root(), art, kWarriorAndWizard); });
        REQUIRE_FALSE(run.failure);
        CHECK(loaded);
        CHECK(run.warnings.empty());
        // Every legend item asks Dark Legacy for its picture, in realm order.
        CHECK(selectLookups(art) == asked({"SCIMITAR", "ICE_AX", "LAMP", "BILLOWS", "SOUL_SAVIOR",
                                           "BOOK", "FIRE_SCROLL", "LANTERN", "JAVILIN"}));
        checkStandIns(shownBy(look));
    }
    SECTION("the bank cut short") {
        const Fixture files("dreamcast-hud-powerups-short");
        files.truncate("powerups/objects.rom");
        LegendsDreamcastHudPainter look;
        bool loaded = false;
        const auto run = test::logWhile(
            [&] { loaded = look.load(device, files.root(), art, kWarriorAndWizard); });
        REQUIRE_FALSE(run.failure);
        CHECK(loaded);
        REQUIRE(run.warnings.size() == 1);
        CHECK(run.warnings.front().find("powerups") != std::string::npos);
        checkStandIns(shownBy(look));
    }
    SECTION("an icon the bank lacks") {
        const Fixture files("dreamcast-hud-powerups-lacking");
        std::vector<test::DreamcastPicture> pictures;
        for (const test::DreamcastPicture& picture : Fixture::kPowerupPictures) {
            if (picture.name != "QUEST_ICON5") {
                pictures.push_back(picture);
            }
        }
        files.writeBank(Fixture::kPowerupsFolder, pictures);
        LegendsDreamcastHudPainter look;
        CHECK(look.load(device, files.root(), art, kWarriorAndWizard));
        CHECK(selectLookups(art) ==
              asked({"SOUL_SAVIOR", "BOOK", "FIRE_SCROLL", "LANTERN", "JAVILIN"}));
        const std::vector<Quad> quads = shownBy(look);
        CHECK(picturesIn(quads, "QUEST_ICON1", 0).size() == 1);
        CHECK(picturesIn(quads, "QUEST_ICON5").empty());
        const std::vector<Quad> savior = lent.drawn(quads, "SOUL_SAVIOR");
        REQUIRE(savior.size() == 1);
        checkRect(savior.front().area, {8 + 38 + 4, 367, 8, 16});
    }
}

TEST_CASE("a dying player's box keeps what it carries and its runestones, and loses its arch, its "
          "bosses' keys and its legend items",
          "[legends][dreamcast-hud]") {
    LoadedLook dreamcast("dreamcast-hud-dying", kWarriorAndWizard);
    // A yellow warrior with all a box shows: two keys, a yellow potion, a full meter, three runes,
    // the first boss's key and two legend items, one the Dreamcast's own and one Dark Legacy's,
    // its save holding them as its box shows them.
    Inventory carried;
    carried.keys = 2;
    carried.potions = {3};
    Holder holder(carried, 0, kWarriorClass, 0);
    Relics& relics = holder.runtime.actor.save().progress().relics;
    relics.runes = 0x7;
    relics.shards = 0x1;
    relics.legends = legendsOf({2, 7});
    HudSeat warrior = holder.shown();
    warrior.box.keys = 2;
    warrior.box.potions = 1;
    warrior.box.potionKind = 3;
    warrior.box.runes = 0x7;
    warrior.box.bossKeys = 0x1;
    warrior.legends = legendsOf({2, 7});
    warrior.turbo = 100.0f;
    HudView view = dreamcast.viewOf({warrior, {}, {}, {}});
    view.relicsShown = true;
    prepareAt(dreamcast, view, 0);
    CHECK(boxOrder(dreamcast.boxes(view), 0, dreamcast.lent) ==
          Names{"BK_RUNE_STONE_02", "BTMBK_YELO", "WAR_NAME", "BTMBK_LEVL", "BTMBK_GOLD",
                "BTMBK_HELTH", "KEY_ICON", "POTION_ICON_YEL", "TRBO_FULL", "TURBO_GLOW",
                "SM_RUNE_BLU_01", "SM_RUNE_BLU_02", "SM_RUNE_BLU_03", "SM_KEY_BLU", "BOOK",
                "QUEST_ICON2"});

    // Dying, PartyHud::status empties the box, and PlayerHealth::fall the meter. FUN_8c0bbe40
    // keeps the Dreamcast's panel through the death (state 11): FUN_8c0c16e0 goes on drawing the
    // keys and the potion carried, and nothing hides the runestones until FUN_8c0bc040 makes the
    // box anew. FUN_8c0c2120 hides the bosses' keys outside states 1, 5, 7 and 8, and
    // FUN_8c0c16e0 shows the legend items, still held, only in states 1 and 7.
    view.seats[0] = fallen(warrior, false);
    REQUIRE(view.seats[0].runtime == &holder.runtime);
    REQUIRE(view.seats[0].legends == legendsOf({2, 7}));
    prepareAt(dreamcast, view, 1);
    const std::vector<Quad> dying = dreamcast.boxes(view);
    CHECK(boxOrder(dying, 0, dreamcast.lent) ==
          Names{"BK_RUNE_STONE_02", "BTMBK_YELO", "WAR_NAME", "BTMBK_LEVL", "BTMBK_GOLD",
                "BTMBK_HELTH", "KEY_ICON", "POTION_ICON_YEL", "SM_RUNE_BLU_01", "SM_RUNE_BLU_02",
                "SM_RUNE_BLU_03"});
    // The gold, the health gone, and the counts of what is carried, in the seat's ink.
    CHECK(linesOf(dying, Fixture::kScoreFont) ==
          std::vector<Line>{
              lineAt(8 + 3, 365, "1250", kYellowInk), lineAt(8 + 92 - 8, 365, "0", kYellowInk),
              lineAt(8 + 18, 323, "2", kYellowInk), lineAt(8 + 82, 323, "1", kYellowInk)});
    CHECK(dreamcast.lent.drawn(dying, "BOOK").empty());
    CHECK(picturesIn(dying, "QUEST_ICON2").empty());
    // Once the death has played out Dark Legacy drops the keys (PartyPickups::dropKeys), and the
    // box shows none.
    holder.runtime.actor.save().progress().inventory.keys = 0;
    CHECK(picturesIn(dreamcast.boxes(view), "KEY_ICON").empty());
}

TEST_CASE("Dreamcast cards stand on the seat's strip, with Dark Legacy's art where the Dreamcast "
          "has none",
          "[legends][dreamcast-hud]") {
    using Set = test::DarkLegacyLookup::Set;
    // Dark Legacy lends its crystal card, and its gold card, which the Dreamcast has its own of.
    const std::array<test::DarkLegacyLookup, 2> lend{
        {{Set::Static, "CRYSTAL"}, {Set::Static, "GOLD"}}};
    // A red warrior in seat 0 and a green wizard in seat 2: each strip takes its costume's
    // colour.
    const std::array<HudMember, 2> party{{
        {.seat = 0, .classIndex = kWarriorClass, .color = 2},
        {.seat = 2, .classIndex = kWizardClass, .color = 3},
    }};
    LoadedLook dreamcast("dreamcast-hud-cards", party, lend);
    const Texture* crystal = dreamcast.art.staticArt(PickupHud::kCrystalCard);
    const Texture* darkLegacyGold = dreamcast.art.staticArt("GOLD");
    REQUIRE(crystal != nullptr);
    REQUIRE(darkLegacyGold != nullptr);
    // Seat 0 took junk, its card risen to rest over the box; seat 2 a crystal, its card still
    // rising, as Dark Legacy's cards move.
    PickupHud pickups;
    pickups.addCard(0, "JUNK");
    pickups.step(PickupHud::kCardStartY - PickupHud::kCardRestY, 0);
    pickups.addCard(2, PickupHud::kCrystalCard);
    pickups.step(20, 0);
    REQUIRE(pickups.cards().size() == 2);
    REQUIRE(pickups.cards()[0].y == 304);
    REQUIRE(pickups.cards()[1].y == 364);
    HudView view =
        dreamcast.viewOf({standing(kWarriorClass, 2), {}, standing(kWizardClass, 3), {}});
    view.pickups = &pickups;
    const std::vector<Quad> quads = dreamcast.boxes(view);

    // FUN_8c0b76a0: the taker's coloured strip, S3_W<colour>, from 8 left of its column, with
    // the card under it. Junk shows the Dreamcast's GOLD card, 128 square, of which the screen
    // shows the top half; the crystal, which the Dreamcast has no card of, Dark Legacy's own,
    // 128 by 64 as Dark Legacy's boxes draw it. They come after every box.
    REQUIRE(quads.size() > 4);
    const std::span<const Quad> cards = std::span{quads}.last(4);
    CHECK(Fixture::pictureOf(cards[0].texture).name == "S3_WRED");
    checkRect(cards[0].area, {0, 304, 128, 16});
    CHECK(Fixture::pictureOf(cards[1].texture).name == "GOLD");
    CHECK(Fixture::pictureOf(cards[1].texture).folder == Fixture::kStaticFolder);
    checkRect(cards[1].area, {0, 320, 128, 128});
    CHECK(Fixture::pictureOf(cards[2].texture).name == "S3_WGREEN");
    checkRect(cards[2].area, {288, 364, 128, 16});
    CHECK(cards[3].texture == crystal);
    checkRect(cards[3].area, {288, 380, 128, 64});
    for (const Quad& card : cards) {
        CHECK(card.color == Color::white());
    }
    // Before the cards, the boxes alone: a standing player's box has no coloured strip.
    for (const Quad& quad : std::span{quads}.first(quads.size() - cards.size())) {
        CHECK_FALSE(Fixture::pictureOf(quad.texture).name.starts_with("S3_W"));
    }
    // Dark Legacy's gold card never stands for the Dreamcast's.
    for (const Quad& quad : quads) {
        CHECK(quad.texture != darkLegacyGold);
    }
}

TEST_CASE("the ticker writes Dark Legacy's counts in the Dreamcast's small font",
          "[legends][dreamcast-hud]") {
    using Set = test::DarkLegacyLookup::Set;
    const std::array<test::DarkLegacyLookup, 1> lend{
        {{Set::Count, std::string{PickupHud::crystalIcon(2)}}}};
    LoadedLook dreamcast("dreamcast-hud-ticker", kWarriorAndWizard, lend);
    const Texture* crystal = dreamcast.art.countArt(PickupHud::crystalIcon(2));
    REQUIRE(crystal != nullptr);
    // Seat 1 gathers a crystal and a serpent's fangs, whose icon Dark Legacy does not lend, and
    // its card rises over the box.
    PickupHud pickups;
    pickups.addCard(1, "MAGIC");
    pickups.showCount(1, PickupHud::crystalIcon(2), 3, 15);
    pickups.showCount(1, PickupHud::kGargoyleIcons[0], 2, 12);
    HudView view =
        dreamcast.viewOf({standing(kWarriorClass, 0), standing(kWizardClass, 1), {}, {}});
    view.pickups = &pickups;
    constexpr f32 kScale = 1.5f;
    CHECK(Layout::kTickerScale == kScale);

    // The Dreamcast has no ticker: Dark Legacy's counts, in its rows, its icon then the count in
    // font 1 at Dark Legacy's size, white, centred on the box, the next stacked 20 above.
    const auto checkTicker = [&](s32 bottom) {
        const std::vector<Quad> quads = dreamcast.boxes(view);
        // Four glyphs of font 1's six, at one and a half: 36 across, after the icon and 4.
        const s32 width = 16 + 4 + measure(Fixture::kSmallFont, "3/15", kScale);
        REQUIRE(width == 56);
        const s32 left = 168 - (width / 2);
        std::optional<usize> icon;
        std::optional<usize> card;
        for (usize index = 0; index < quads.size(); ++index) {
            if (quads[index].texture == crystal) {
                REQUIRE_FALSE(icon);
                icon = index;
            }
            if (Fixture::pictureOf(quads[index].texture).name == "MAGIC") {
                card = index;
            }
        }
        REQUIRE(icon);
        REQUIRE(card);
        checkRect(quads[*icon].area, {static_cast<f32>(left), static_cast<f32>(bottom), 16, 16});
        CHECK(quads[*icon].color == Color::white());
        // Over the cards.
        CHECK(*icon > *card);
        std::vector<Line> counts;
        for (const Line& line : linesOf(quads, Fixture::kSmallFont, kScale)) {
            if (line.y < static_cast<f32>(StatusBoxPainter::kBarY)) {
                counts.push_back(line);
            }
        }
        CHECK(counts == std::vector<Line>{
                            lineAt(left + 20, bottom + 4, "3/15", Color::white(), kScale),
                            lineAt(left + 20, bottom - 20 + 4, "2/12", Color::white(), kScale)});
    };
    CHECK(PickupHud::countRowY(std::nullopt) == 288);
    checkTicker(288);

    // While seat 1's selector is up its label rises over the box, and the counts stand above it.
    Inventory inventory;
    inventory.powerups[0] = PowerupSlot{30, powerup::kSpecial, 0, powerup::kInvisible, false};
    PowerupSelector selector;
    selector.step(SelectorInput{.up = true}, inventory,
                  PowerupSelector::kSlide / PowerupSelector::kSlidePerTick);
    selector.step({}, inventory, 1);
    REQUIRE(selector.showing());
    view.seats[1].selector = &selector;
    checkTicker(PickupHud::countRowY(selector.labelY(StatusBoxPainter::kY)));
}

TEST_CASE("IT marks the tagged player's box", "[legends][dreamcast-hud]") {
    LoadedLook dreamcast("dreamcast-hud-it", kWarriorAndWizard);
    HudSeat tagged = standing(kWizardClass, 1);
    tagged.it = true;
    tagged.legends = legendsOf({2});
    HudView view = dreamcast.viewOf({standing(kWarriorClass, 0), tagged, {}, {}});
    std::vector<Quad> quads = dreamcast.boxes(view);
    // FUN_8c0c2300: the static set's IT, 64 square, squeezed into 32 at (X+26, 349), over the
    // rest of the tagged player's box and no one else's.
    const std::vector<Quad> badge = picturesIn(quads, "IT");
    REQUIRE(badge.size() == 1);
    CHECK(Fixture::pictureOf(badge.front().texture).folder == Fixture::kStaticFolder);
    checkRect(badge.front().area, {120 + 26, 349, 32, 32});
    checkRect(badge.front().uv, {0, 0, 1, 1});
    CHECK(badge.front().color == Color::white());
    CHECK(boxOrder(quads, 1, dreamcast.lent) == Names{"BK_RUNE_STONE_02", "BTMBK_BLUE", "WIZ_NAME",
                                                      "BTMBK_LEVL", "BTMBK_GOLD", "BTMBK_HELTH",
                                                      "QUEST_ICON2", "IT"});
    // No longer IT, the box shows none.
    view.seats[1].it = false;
    quads = dreamcast.boxes(view);
    CHECK(picturesIn(quads, "IT").empty());
}

TEST_CASE("WAIT HERE blinks with its words in a waiting player's box, never alone",
          "[legends][dreamcast-hud]") {
    LoadedLook dreamcast("dreamcast-hud-wait-here", kWarriorAndWizard);
    HudSeat waiting = standing(kWarriorClass, 0);
    waiting.waitingAtExit = true;
    HudView view = dreamcast.viewOf({waiting, standing(kWizardClass, 1), {}, {}});
    view.joined = 2;
    /** What the boxes show of WAIT HERE at the look's step `step`: its scrolls, the lines
     * written in its brown, and seat 0's pictures in turn. */
    struct Shown {
        std::vector<Quad> scrolls;
        std::vector<Line> words;
        Names order;
        bool wordsOverScroll = true;
    };
    const auto shownAt = [&](s32 step) {
        prepareAt(dreamcast, view, step);
        const std::vector<Quad> quads = dreamcast.boxes(view);
        Shown shown;
        shown.scrolls = picturesIn(quads, "TEXTSCROLL");
        for (const Line& line : linesOf(quads, Fixture::kSmallFont)) {
            if (line.color == Layout::kWaitHereInk) {
                shown.words.push_back(line);
            }
        }
        shown.order = pictureOrder(quads, 0);
        std::optional<usize> scroll;
        std::optional<usize> firstWord;
        for (usize index = 0; index < quads.size(); ++index) {
            if (!isText(quads[index]) &&
                Fixture::pictureOf(quads[index].texture).name == "TEXTSCROLL") {
                scroll = index;
            }
            if (!firstWord && isText(quads[index]) && quads[index].color == Layout::kWaitHereInk) {
                firstWord = index;
            }
        }
        shown.wordsOverScroll = !scroll || !firstWord || *scroll < *firstWord;
        return shown;
    };
    // FUN_8c0c16e0: TEXTSCROLL squeezed into 112x16 from X-8 at 324, and WAIT HERE... over it in
    // font 1 in its brown (8c0c1af4) from X+8 at 328, both together, four steps on and four off.
    CHECK(Layout::kWaitHereInk == Color::rgba(0x16, 0x0C, 0x03));
    const Names panel{"BK_RUNE_STONE_02", "BTMBK_YELO", "WAR_NAME",
                      "BTMBK_LEVL",       "BTMBK_GOLD", "BTMBK_HELTH"};
    Names withScroll = panel;
    withScroll.emplace_back("TEXTSCROLL");
    for (s32 step = 0; step < 16; ++step) {
        CAPTURE(step);
        const Shown shown = shownAt(step);
        if ((step & 4) == 0) {
            CHECK(shown.scrolls.empty());
            CHECK(shown.words.empty());
            CHECK(shown.order == panel);
            continue;
        }
        REQUIRE(shown.scrolls.size() == 1);
        checkRect(shown.scrolls.front().area, {0, 324, 112, 16});
        checkRect(shown.scrolls.front().uv, {0, 0, 1, 1});
        CHECK(shown.scrolls.front().color == Color::white());
        CHECK(shown.words ==
              std::vector<Line>{lineAt(8 + 8, 328, "WAIT HERE...", Layout::kWaitHereInk)});
        CHECK(shown.wordsOverScroll);
        // Over the rest of the waiting player's box.
        CHECK(shown.order == withScroll);
    }
    // A player alone in the game waits for nobody: never (*8c287bb0 > 1).
    view.joined = 1;
    for (s32 step = 0; step < 8; ++step) {
        CAPTURE(step);
        const Shown shown = shownAt(step);
        CHECK(shown.scrolls.empty());
        CHECK(shown.words.empty());
    }
    // Nor does a player off the exit.
    view.joined = 2;
    view.seats[0].waitingAtExit = false;
    for (s32 step = 4; step < 8; ++step) {
        CAPTURE(step);
        const Shown shown = shownAt(step);
        CHECK(shown.scrolls.empty());
        CHECK(shown.words.empty());
    }
}

TEST_CASE("a fallen player is asked WAIT IN TOWER? on the kind-0 panel, with YES marked, and NO "
          "when it may quit",
          "[legends][dreamcast-hud]") {
    const std::array<HudMember, 2> party{{
        {.seat = 0, .classIndex = kWarriorClass, .color = 2},
        {.seat = 3, .classIndex = kWizardClass, .color = 1},
    }};
    LoadedLook dreamcast("dreamcast-hud-prompt", party);
    // Seat 0, red, may quit the game; seat 3, blue, plays online, where only waiting is offered.
    const HudSeat asked = fallen(standing(kWarriorClass, 2), true, true);
    HudSeat online = fallen(standing(kWizardClass, 1), true, true);
    online.box.quitOffered = false;
    const std::vector<Quad> quads = dreamcast.boxes(dreamcast.viewOf({asked, {}, {}, online}));
    // FUN_8c0bc040 rebuilds a falling player's box as kind 0, the coloured strip and panel alone.
    CHECK(pictureOrder(quads, 0) == Names{"S3_WRED", "S4_WRED"});
    CHECK(pictureOrder(quads, 3) == Names{"S3_WBLUE", "S4_WBLUE"});
    // FUN_8c0bda40 asks in font 1, each line centred on the box: WAIT (8c1254cc) at 336 and IN
    // TOWER? (8c1254d4) at 344 in the seat's ink, YES (8c1254f4) at 356 in red, as its cursor
    // starts on it, and NO (8c1254f8) at 368 in the seat's ink, which 8c0bf42e stores over the
    // table's white. Online it never offers NO; and the box does not say IN TOWER while it asks.
    CHECK(Layout::kPromptChosenInk == Color::rgba(0xFF, 0x00, 0x00));
    CHECK(linesOf(quads, Fixture::kSmallFont) ==
          std::vector<Line>{
              centredAt(Fixture::kSmallFont, 56, 336, "WAIT", kRedInk),
              centredAt(Fixture::kSmallFont, 56, 344, "IN TOWER?", kRedInk),
              centredAt(Fixture::kSmallFont, 56, 356, "YES", Layout::kPromptChosenInk),
              centredAt(Fixture::kSmallFont, 56, 368, "NO", kRedInk),
              centredAt(Fixture::kSmallFont, 456, 336, "WAIT", kBlueInk),
              centredAt(Fixture::kSmallFont, 456, 344, "IN TOWER?", kBlueInk),
              centredAt(Fixture::kSmallFont, 456, 356, "YES", Layout::kPromptChosenInk)});
}

TEST_CASE("the Dreamcast look takes every picture it borrows as it opens",
          "[legends][dreamcast-hud]") {
    using Set = test::DarkLegacyLookup::Set;
    const Fixture files("dreamcast-hud-borrowed");
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    // Dark Legacy's art stands ready to lend what a look might ask of it.
    struct Lent {
        test::DarkLegacyLookup lookup;
        const Texture* picture = nullptr;
    };
    std::vector<Lent> lent;
    for (const Set set : {Set::Static, Set::Select, Set::Count, Set::Inventory}) {
        for (const std::string_view name : {"RUNE13", "SM_KEY_BLU", "SM_RUNE_BLU_01", "SCIMITAR",
                                            "BOOK", "GOLD", "SHP_XRAY_GLASSES"}) {
            lent.push_back({{set, std::string{name}}, &art.give(set, name)});
        }
    }
    StringTable strings;
    REQUIRE(strings.load(test::dataDirectory() / "text", "en"));
    LegendsDreamcastHudPainter look;
    REQUIRE(look.load(device, files.root(), art, kWarriorAndWizard));
    // As it opens it takes SELECT's pictures of the legend items the Dreamcast has no icon of,
    // STATIC's cards of the pickups it has no card of, and every icon a count may show, and
    // nothing else of Dark Legacy's: the boxes, the runestones, the keys, the thirteenth rune,
    // the other legend items and the other cards are the Dreamcast's own art.
    std::vector<test::DarkLegacyLookup> borrowed;
    for (const std::string_view name : {"BOOK", "FIRE_SCROLL", "LANTERN", "JAVILIN"}) {
        borrowed.push_back({Set::Select, std::string{name}});
    }
    for (const std::string_view name : {"RUNESTONE", "LEGEND", "CRYSTAL", "GOLDNICON"}) {
        borrowed.push_back({Set::Static, std::string{name}});
    }
    for (const std::string& icon : PickupHud::countIcons()) {
        borrowed.push_back({Set::Count, icon});
    }
    CHECK(art.lookups == borrowed);
    art.lookups.clear();
    // It uploads what it draws of the static set, with the fonts' sheets, the party's panels and
    // banners, and the legend items' icons and the hourglass of the powerups bank: nothing else
    // it reads.
    CHECK(device.texturesCreated ==
          Fixture::kStaticPictures.size() - 1 + 4 + Fixture::kPowerupPictures.size() - 1);
    // A boss meter, its pictures up as its archive keeps them once drawn.
    TextureSet meterPictures = meterArt(BossMeter::kPieceWidth, 64);
    const std::array<HealthMeterReading, 1> boss{meterReading(2, 46, 72, 1875, 2500)};
    BossMeters meters;
    meters.bind(boss, &meterPictures);
    meters.meter(0).preload(device);
    const u32 uploaded = device.texturesCreated;

    // Every seat state and every part of the HUD, over a blink's round of steps: nothing is
    // looked up or uploaded while drawing. Seat 0 carries everything a box shows, among it a
    // legend item the Dreamcast has no icon of in the place of one it has, and a powerup its
    // selector names over the box; it is IT and waits at an exit, under a card of each kind and
    // a count, with its help over its head. The finder and the hourglass show, and a boss's
    // meter.
    Inventory carried;
    carried.powerups[0] = PowerupSlot{30, powerup::kSpecial, 0, powerup::kInvisible, true};
    Holder holder(carried, 0, kWarriorClass, 0);
    holder.open();
    HudSeat carrying = standing(kWarriorClass, 0, "A", 250);
    carrying.runtime = &holder.runtime;
    carrying.selector = &holder.selector;
    carrying.box.keys = 2;
    carrying.box.potions = 1;
    carrying.box.potionKind = 3;
    carrying.box.runes = Relics::kAllRunes;
    carrying.box.bossKeys = 0xFF;
    carrying.legends = legendsOf({2, 7});
    carrying.turbo = 100.0f;
    carrying.it = true;
    carrying.waitingAtExit = true;
    const Departed gone(3, kWizardClass, 3, "D");
    PickupHud pickups;
    pickups.addCard(0, "GOLD");
    pickups.addCard(0, PickupHud::kCrystalCard);
    pickups.showCount(0, PickupHud::crystalIcon(2), 3, 15);
    const std::vector<std::string> lines{"WATCH OUT"};
    HudView view;
    view.seats = {carrying, fallen(standing(kWizardClass, 1), true, true),
                  fallen(standing(kWarriorClass, 0), true), gone.seat()};
    view.strings = &strings;
    view.relicsShown = true;
    view.joined = 3;
    view.pickups = &pickups;
    view.help =
        HudHelp{.lines = lines, .player = 0, .color = 0, .head = {56, 300}, .anchored = true};
    view.runeFill = 0.5f;
    view.hourglass = ChallengeHud::Look{.elapsed = 0.5f, .fallingFrame = 2};
    view.bossMeters = &meters;
    view.bossCount = hudBossReadingsOf(boss, view.bossReadings);
    constexpr s32 kSteps = 8;
    for (s32 step = 0; step < kSteps; ++step) {
        view.seconds = secondsAt(step);
        Canvas canvas;
        canvas.begin(device, Mat4{1.0f});
        HudFrame hud{canvas, device, art};
        look.prepare(hud, view);
        look.drawStatus(hud, view);
        look.drawRuneMeter(hud, view);
        look.drawHourglass(hud, view);
        look.drawBossMeters(hud, view);
        look.drawSelectors(hud, view);
        look.drawHelp(hud, view);
        canvas.end();
    }
    CHECK(art.lookups.empty());
    CHECK(device.texturesCreated == uploaded);
    REQUIRE_FALSE(device.draws.empty());
    // Of what Dark Legacy lends, only the book it borrowed is drawn; its own runestones, keys,
    // thirteenth rune and gold card never are.
    const test::DarkLegacyLookup book{Set::Select, "BOOK"};
    usize books = 0;
    std::vector<std::string_view> drawn;
    for (const Quad& quad : quadsOf(device)) {
        for (const Lent& given : lent) {
            if (quad.texture == given.picture) {
                CHECK(given.lookup == book);
                ++books;
            }
        }
        CHECK(Fixture::pictureOf(quad.texture).name != Fixture::kUndrawn);
        CHECK(Fixture::pictureOf(quad.texture).name != Fixture::kUndrawnPowerup);
        drawn.push_back(Fixture::pictureOf(quad.texture).name);
    }
    // Once a frame, in its own place.
    CHECK(books == static_cast<usize>(kSteps));
    // The badge, the blinking scroll, the Dreamcast's card, the finder, the hourglass and the
    // help's scroll were all drawn, and so was the meter.
    for (const std::string_view name : {"IT", "TEXTSCROLL", "GOLD", "THERMBASE", "THERMCOL",
                                        "TIMER", "TIMER_SAND", "SAND_ANIM02", "YEL_TEXTSCROLL"}) {
        CAPTURE(name);
        CHECK(std::ranges::find(drawn, name) != drawn.end());
    }
    CHECK(std::ranges::any_of(device.draws, [&](const test::RecordedDraw& draw) {
        return draw.texture == meterPicture(meterPictures, device, "METER_FG1");
    }));
    // Of Dark Legacy's look, nothing.
    CHECK(art.record.calls.empty());
}

TEST_CASE("the Dreamcast look draws every part in its own art, and its hourglass in Dark "
          "Legacy's only without its own",
          "[legends][dreamcast-hud]") {
    /** What Dark Legacy's look was asked while the Dreamcast look, loaded from `files`, drew a
     * frame of every part. */
    const auto darkLegacyCalls = [](const Fixture& files) {
        test::FakeRenderDevice device;
        test::FakeDarkLegacyArt art;
        LegendsDreamcastHudPainter look;
        REQUIRE(look.load(device, files.root(), art, kWarriorAndWizard));
        HudView view;
        view.seats[0] = standing(kWarriorClass, 0);
        Canvas canvas;
        canvas.begin(device, Mat4{1.0f});
        HudFrame frame{canvas, device, art};
        look.prepare(frame, view);
        look.drawStatus(frame, view);
        look.drawRuneMeter(frame, view);
        look.drawHourglass(frame, view);
        look.drawBossMeters(frame, view);
        look.drawSelectors(frame, view);
        look.drawHelp(frame, view);
        canvas.end();
        return art.record.calls;
    };
    SECTION("its own art whole") {
        // The boxes, the runestone finder, the hourglass, the boss meters, the selector labels
        // and the help are the Dreamcast's own: Dark Legacy's look is asked nothing.
        const Fixture files("dreamcast-hud-own-look");
        CHECK(darkLegacyCalls(files).empty());
    }
    SECTION("no powerups bank") {
        // Without the bank's hourglass, the hourglass is Dark Legacy's, prepared as its look
        // prepares it, and nothing else is.
        const Fixture files("dreamcast-hud-own-look-no-timer");
        REQUIRE(std::filesystem::remove(files.root() / "powerups/objects.rom"));
        const std::string darkLegacy{kDarkLegacyHudStyle};
        CHECK(darkLegacyCalls(files) ==
              std::vector<test::FakeHudCall>{
                  {.look = darkLegacy, .part = test::FakeHudPart::Prepare, .seat = -1},
                  {.look = darkLegacy, .part = test::FakeHudPart::Hourglass, .seat = -1}});
    }
}

TEST_CASE("the Dreamcast runestone finder stands at the top right, its column rising straight "
          "with the closeness",
          "[legends][dreamcast-hud]") {
    LoadedLook dreamcast("dreamcast-hud-finder", kWarriorAndWizard);
    HudView view = dreamcast.viewOf({});
    // Hidden, it draws nothing.
    CHECK(dreamcast.drawn(view, &LegendsDreamcastHudPainter::drawRuneMeter).empty());

    // FUN_8c09b360: the static set's THERMBASE whole at (392, 1), then THERMCOL over it. At Dark
    // Legacy's fill of a quarter the closeness is a half, and the column stands half its way up,
    // where Dark Legacy's own column stands a quarter of its way.
    view.runeFill = 0.25f;
    const std::vector<Quad> quads =
        dreamcast.drawn(view, &LegendsDreamcastHudPainter::drawRuneMeter);
    REQUIRE(quads.size() == 2);
    CHECK(Fixture::pictureOf(quads[0].texture).name == "THERMBASE");
    checkRect(quads[0].area, {392, 1, 128, 128});
    checkRect(quads[0].uv, {0, 0, 1, 1});
    CHECK(Fixture::pictureOf(quads[1].texture).name == "THERMCOL");
    const RuneMeter::Column half = RuneMeter::column(0.5f, 128);
    checkRect(quads[1].area, half.area);
    checkRect(quads[1].uv, half.uv);
    checkRect(quads[1].area, {392, 65, 128, 64});
    checkRect(RuneMeter::column(0.25f, 128).area, {392, 84, 128, 45});
    for (const Quad& quad : quads) {
        CHECK(Fixture::pictureOf(quad.texture).folder == Fixture::kStaticFolder);
        CHECK(quad.color == Color::white());
    }
    // Full, the column stands its whole height.
    view.runeFill = 1.0f;
    const std::vector<Quad> full =
        dreamcast.drawn(view, &LegendsDreamcastHudPainter::drawRuneMeter);
    REQUIRE(full.size() == 2);
    checkRect(full[1].area, {392, 29, 128, 100});
    // The Dreamcast's own art: Dark Legacy's look is not asked.
    CHECK(dreamcast.art.record.count(kDarkLegacyHudStyle, test::FakeHudPart::RuneMeter) == 0);
}

TEST_CASE("the Dreamcast hourglass uses its own art in Dark Legacy's places",
          "[legends][dreamcast-hud]") {
    LoadedLook dreamcast("dreamcast-hud-hourglass", kWarriorAndWizard);
    HudView view = dreamcast.viewOf({});
    // No hourglass, nothing drawn.
    CHECK(dreamcast.drawn(view, &LegendsDreamcastHudPainter::drawHourglass).empty());

    // FUN_8c09b640 stands the powerups bank's TIMER at (1, 1), its two TIMER_SAND crops from 24
    // and 106 down, 41 and 23 rows tall from v 23/128 and 105/128, and the grains at (63, 58):
    // Dark Legacy's hourglass's own numbers.
    checkRect(ChallengeHud::kFrameArea, {1, 1, 128, 128});
    checkRect(ChallengeHud::kFallingArea, {63, 58, 8, 32});
    const ChallengeHud::Sand start = ChallengeHud::sandAt(0);
    checkRect(start.upper, {1, 24, 128, 41});
    checkRect(start.upperUv, {0, 23.0f / 128, 1, 0.5f - (23.0f / 128)});
    checkRect(start.lower, {1, 106, 128, 23});
    checkRect(start.lowerUv, {0, 105.0f / 128, 1, 1 - (105.0f / 128)});
    struct Piece {
        std::string_view picture;
        Rect area;
        Rect uv;
    };
    const auto check = [&](const ChallengeHud::Look& look, const std::vector<Piece>& expected) {
        view.hourglass = look;
        const std::vector<Quad> quads =
            dreamcast.drawn(view, &LegendsDreamcastHudPainter::drawHourglass);
        REQUIRE(quads.size() == expected.size());
        for (usize index = 0; index < expected.size(); ++index) {
            CAPTURE(index);
            const Fixture::Found found = Fixture::pictureOf(quads[index].texture);
            CHECK(found.folder == Fixture::kPowerupsFolder);
            CHECK(found.name == expected[index].picture);
            checkRect(quads[index].area, expected[index].area);
            checkRect(quads[index].uv, expected[index].uv);
            // Opaque: FUN_8c09b640 sets no transparency on them.
            CHECK(quads[index].color == Color::white());
        }
    };
    const Rect whole{0, 0, 1, 1};
    // The blits deepest first, the glass (3.0), the grains (2.0), then the sand (1.0); the
    // grains' frame is Dark Legacy's falling counter's, held to the five.
    const ChallengeHud::Sand half = ChallengeHud::sandAt(0.5f);
    check({.elapsed = 0.5f, .fallingFrame = 7}, {{"TIMER", ChallengeHud::kFrameArea, whole},
                                                 {"SAND_ANIM02", ChallengeHud::kFallingArea, whole},
                                                 {"TIMER_SAND", half.upper, half.upperUv},
                                                 {"TIMER_SAND", half.lower, half.lowerUv}});
    // While the clock stands still the grains are hidden, and the sand stays where it is.
    check({.elapsed = 0.5f, .fallingFrame = -1}, {{"TIMER", ChallengeHud::kFrameArea, whole},
                                                  {"TIMER_SAND", half.upper, half.upperUv},
                                                  {"TIMER_SAND", half.lower, half.lowerUv}});
    // Past the whole time it keeps to the glass's end; with no number for it, nothing shows.
    const ChallengeHud::Sand end = ChallengeHud::sandAt(1);
    check({.elapsed = 3, .fallingFrame = 0}, {{"TIMER", ChallengeHud::kFrameArea, whole},
                                              {"SAND_ANIM00", ChallengeHud::kFallingArea, whole},
                                              {"TIMER_SAND", end.upper, end.upperUv},
                                              {"TIMER_SAND", end.lower, end.lowerUv}});
    check({.elapsed = std::numeric_limits<f32>::quiet_NaN(), .fallingFrame = 0}, {});
    // The Dreamcast's own art: Dark Legacy's look is not asked.
    CHECK(dreamcast.art.record.count(kDarkLegacyHudStyle, test::FakeHudPart::Hourglass) == 0);
}

TEST_CASE("without its hourglass art the Dreamcast look draws Dark Legacy's",
          "[legends][dreamcast-hud]") {
    // Dark Legacy's own look lends its art, and draws the hourglass it is given.
    test::FakeRenderDevice device;
    DarkLegacyHudPainter darkLegacy;
    ItemArchive archive = darkLegacyHourglass();
    ChallengeHud stopTime;
    REQUIRE(stopTime.bind(device, archive));
    HudView view;
    view.hourglass = stopTime.look(45, 60, true);
    view.hourglassArt = &stopTime;
    REQUIRE(view.hourglass->fallingFrame >= 0);
    const auto drawsOf = [&](HudPainter& look) {
        device.draws.clear();
        Canvas canvas;
        canvas.begin(device, Mat4{1.0f});
        HudFrame frame{canvas, device, darkLegacy};
        look.drawHourglass(frame, view);
        canvas.end();
        return device.draws;
    };
    const std::vector<test::RecordedDraw> expected = drawsOf(darkLegacy);
    REQUIRE(expected.size() == 3);
    const auto check = [&](const Fixture& files, usize warnings) {
        LegendsDreamcastHudPainter look;
        bool loaded = false;
        const auto run = test::logWhile(
            [&] { loaded = look.load(device, files.root(), darkLegacy, kWarriorAndWizard); });
        REQUIRE_FALSE(run.failure);
        CHECK(loaded);
        REQUIRE(run.warnings.size() == warnings);
        for (const std::string& warning : run.warnings) {
            CHECK(warning.find(LegendsDreamcastHudPainter::kPowerupsFolder) != std::string::npos);
        }
        const std::vector<test::RecordedDraw> drawn = drawsOf(look);
        REQUIRE(drawn.size() == expected.size());
        for (usize index = 0; index < drawn.size(); ++index) {
            CAPTURE(index);
            test::checkRecordedDraw(drawn[index], expected[index]);
        }
        return run;
    };
    SECTION("no powerups bank") {
        const Fixture files("dreamcast-hud-no-timer");
        REQUIRE(std::filesystem::remove(files.root() / "powerups/objects.rom"));
        check(files, 0);
    }
    SECTION("a powerups bank cut short") {
        const Fixture files("dreamcast-hud-short-timer");
        files.truncate("powerups/objects.rom");
        check(files, 1);
    }
    SECTION("a powerups bank without a frame of the grains") {
        const Fixture files("dreamcast-hud-incomplete-timer");
        std::vector<test::DreamcastPicture> pictures;
        for (const test::DreamcastPicture& picture : Fixture::kPowerupPictures) {
            if (picture.name != "SAND_ANIM03") {
                pictures.push_back(picture);
            }
        }
        files.writeBank(Fixture::kPowerupsFolder, pictures);
        const auto run = check(files, 1);
        CHECK(run.warnings.front().find("SAND_ANIM03") != std::string::npos);
    }
}

TEST_CASE("Dreamcast boss meters hang at the top, opaque, cropped as Dark Legacy's",
          "[legends][dreamcast-hud]") {
    LoadedLook dreamcast("dreamcast-hud-meters", kWarriorAndWizard);
    HudView view = dreamcast.viewOf({});
    // No meter, nothing drawn.
    CHECK(dreamcast.drawn(view, &LegendsDreamcastHudPainter::drawBossMeters).empty());
    struct Piece {
        const Texture* texture = nullptr;
        Rect area;
        Rect uv;
    };
    const auto check = [&](const std::vector<Quad>& quads, const std::vector<Piece>& expected) {
        REQUIRE(quads.size() == expected.size());
        for (usize index = 0; index < expected.size(); ++index) {
            CAPTURE(index);
            CHECK(quads[index].texture == expected[index].texture);
            checkRect(quads[index].area, expected[index].area);
            checkRect(quads[index].uv, expected[index].uv);
            // Opaque and never tinted.
            CHECK(quads[index].color == Color::white());
        }
    };

    // A boss of two strips at three quarters of its health, frozen: FUN_8c071560 hangs each
    // strip, 256 across, from the frame's top (its blits' y 383). The fills are Dark Legacy's
    // crop: the first strip whole, and half of what the second's tail of 72 leaves of it.
    TextureSet art = meterArt(BossMeter::kPieceWidth, 64);
    const std::array<HealthMeterReading, 1> boss{meterReading(2, 46, 72, 1875, 2500)};
    BossMeters meters;
    showMeters(view, meters, boss, art);
    meters.update(0, boss, true, true);
    REQUIRE(meters.meter(0).look().frozen);
    REQUIRE(meters.meter(0).look().widths == std::array<s32, 2>{256, 92});
    const auto picture = [&](std::string_view name) {
        return meterPicture(art, dreamcast.device, name);
    };
    check(dreamcast.drawn(view, &LegendsDreamcastHudPainter::drawBossMeters),
          {{picture("METER_BG1"), {0, 1, 256, 64}, {0, 0, 1, 1}},
           {picture("METER_FG1"), {0, 1, 256, 64}, {0, 0, 1, 1}},
           {picture("METER_BG2"), {256, 1, 256, 64}, {0, 0, 1, 1}},
           {picture("METER_FG2"), {256, 1, 92, 64}, {0, 0, 92.0f / 256, 1}}});

    // Two bosses of a strip each, at half and whole health: each strip from its own meter's
    // left, the second after the first, cropped to its boss's health.
    const std::array<HealthMeterReading, 2> pair{meterReading(1, 12, 20, 250, 500),
                                                 meterReading(1, 12, 20, 500, 500)};
    BossMeters pairMeters;
    showMeters(view, pairMeters, pair, art);
    check(dreamcast.drawn(view, &LegendsDreamcastHudPainter::drawBossMeters),
          {{picture("METER_BG1"), {0, 1, 256, 64}, {0, 0, 1, 1}},
           {picture("METER_FG1"), {0, 1, 112, 64}, {0, 0, 112.0f / 256, 1}},
           {picture("METER_BG1"), {256, 1, 256, 64}, {0, 0, 1, 1}},
           {picture("METER_FG1"), {256, 1, 224, 64}, {0, 0, 224.0f / 256, 1}}});
    // Their bosses dead, the meters go.
    pairMeters.update(0, pair, false, false);
    CHECK(dreamcast.drawn(view, &LegendsDreamcastHudPainter::drawBossMeters).empty());
    // The Dreamcast's own places: Dark Legacy's look is not asked.
    CHECK(dreamcast.art.record.count(kDarkLegacyHudStyle, test::FakeHudPart::BossMeters) == 0);
}

TEST_CASE("a powerup's name shows over its Dreamcast box, in the seat's colour while worn",
          "[legends][dreamcast-hud]") {
    LoadedLook dreamcast("dreamcast-hud-label", kWarriorAndWizard);
    // Dark Legacy's FONT32, by whose line at its label's size the label keeps its bottom.
    const MeasuredFont font32(32);
    REQUIRE(font32.text.lineHeight(PowerupSelector::kLabelScale) == 14);
    // Seat 1, a blue wizard, wears invisibility; seat 2, a red warrior, carries X-ray glasses
    // it does not wear; seat 3, a green warrior, wears gold invulnerability, whose name is wider
    // than the box. Each selector is up, naming its powerup.
    Inventory invisible;
    invisible.powerups[0] = PowerupSlot{30, powerup::kSpecial, 0, powerup::kInvisible, true};
    Inventory glasses;
    glasses.powerups[0] = PowerupSlot{30, powerup::kSpecial, 0, powerup::kXRay, false};
    Inventory gold;
    gold.powerups[0] = PowerupSlot{30, powerup::kArmor, 0, armor::kGoldInvulnerable, true};
    Holder wizard(invisible, 1, kWizardClass, 1);
    Holder warrior(glasses, 2, kWarriorClass, 2);
    Holder knight(gold, 3, kWarriorClass, 3);
    for (Holder* holder : {&wizard, &warrior, &knight}) {
        holder->open();
    }
    // Seat 0's selector is down.
    Holder closed(invisible, 0, kWarriorClass, 0);
    HudView view =
        dreamcast.viewOf({closed.shown(), wizard.shown(), warrior.shown(), knight.shown()});
    view.largeText = &font32.text;
    const std::vector<Quad> quads =
        dreamcast.drawn(view, &LegendsDreamcastHudPainter::drawSelectors);
    CHECK(quadsIn(quads, 0).empty());

    // Each in font 1, centred on its box, its bottom where Dark Legacy's label's is: the label's
    // top as the selector slides it, 29 over the box, and Dark Legacy's line of 14 at 0.45.
    const s32 bottom = wizard.selector.labelY(StatusBoxPainter::kY) + 14;
    REQUIRE(bottom == 291 + 14);
    CHECK(
        linesOf(quadsIn(quads, 1), Fixture::kSmallFont) ==
        std::vector<Line>{centredAt(Fixture::kSmallFont, 168, bottom - 8, "Invisible", kBlueInk)});
    CHECK(linesOf(quadsIn(quads, 2), Fixture::kSmallFont) ==
          std::vector<Line>{
              centredAt(Fixture::kSmallFont, 344, bottom - 8, "X-Ray", Color::white())});
    // The name too wide for the box shrinks to the largest size that keeps it within the box's
    // 96: its glyphs advance 4 of their 6, where at 5 it would be 101 across. Its bottom stays.
    const std::string_view longName = "Gold Invulnerability";
    REQUIRE(measure(Fixture::kSmallFont, longName) > 96);
    const MeasuredFont fontOne(Fixture::kSmallFont);
    const f32 scale = dreamcastLabelScale(fontOne.text, longName);
    CHECK(scale > 0.83f);
    CHECK(scale < 5.0f / 6);
    CHECK(measure(Fixture::kSmallFont, longName, scale) == 82);
    CHECK(measure(Fixture::kSmallFont, longName, 0.84f) == 101);
    const s32 line = static_cast<s32>(8 * scale);
    CHECK(linesOf(quadsIn(quads, 3), Fixture::kSmallFont, scale) ==
          std::vector<Line>{centredAt(Fixture::kSmallFont, 456, bottom - line,
                                      std::string{longName}, kGreenInk, scale)});
    // A name that fits keeps its own size.
    CHECK(dreamcastLabelScale(fontOne.text, "Invisible") == 1);
    // The Dreamcast's own font: Dark Legacy's look is not asked, nor its FONT32 drawn with.
    CHECK(dreamcast.art.record.count(kDarkLegacyHudStyle, test::FakeHudPart::Selectors) == 0);
    for (const Quad& quad : quads) {
        CHECK(Fixture::sheetOf(quad.texture, Fixture::kSmallFont));
    }
}

TEST_CASE("Dreamcast help is written on its player's coloured scroll, in that colour's ink",
          "[legends][dreamcast-hud]") {
    LoadedLook dreamcast("dreamcast-hud-help", kWarriorAndWizard);
    HudView view = dreamcast.viewOf({});
    CHECK(dreamcast.drawn(view, &LegendsDreamcastHudPainter::drawHelp).empty());
    // Dark Legacy's message for seat 1, who wears red, over its head.
    const std::vector<std::string> lines{"WATCH OUT", "AN EXIT"};
    const Vec2 head{300, 250};
    view.help = HudHelp{.lines = lines, .player = 1, .color = 2, .head = head, .anchored = true};
    const LegendsDreamcastHelpBox box = dreamcastHelpBox(lines, head, true, 1);
    const std::vector<Quad> quads = dreamcast.drawn(view, &LegendsDreamcastHudPainter::drawHelp);
    // FUN_8c0e3420: the scroll of the player's colour whole over the box, at transparency 0x40,
    // under the lines.
    REQUIRE_FALSE(quads.empty());
    CHECK(Fixture::pictureOf(quads.front().texture).name == "RED_TEXTSCROLL");
    checkRect(quads.front().area, {static_cast<f32>(box.x), static_cast<f32>(box.y),
                                   static_cast<f32>(box.width), static_cast<f32>(box.height)});
    checkRect(quads.front().uv, {0, 0, 1, 1});
    CHECK(quads.front().color == Color::white().withAlpha(191));
    // The lines in font 1, centred on the scroll, 8 apart down its middle, in red's ink
    // (8c14e7e0).
    const Color red = Color::rgba(0x1F, 0x00, 0x00);
    CHECK(box.textTop == box.y + 12);
    CHECK(linesOf(quads, Fixture::kSmallFont) ==
          std::vector<Line>{
              centredAt(Fixture::kSmallFont, box.textCentre, box.textTop, "WATCH OUT", red),
              centredAt(Fixture::kSmallFont, box.textCentre, box.textTop + 8, "AN EXIT", red)});
    REQUIRE(quads.size() == 1 + 14);
    // Each colour has its scroll and its ink, and a message for nobody the plain scroll in the
    // scroll's brown.
    struct Coloured {
        s32 color = 0;
        std::string_view picture;
        Color ink;
    };
    for (const Coloured& coloured : std::array<Coloured, 5>{{
             {.color = 0, .picture = "YEL_TEXTSCROLL", .ink = Color::rgba(0x1F, 0x1F, 0x00)},
             {.color = 1, .picture = "BLU_TEXTSCROLL", .ink = Color::rgba(0x00, 0x00, 0x1F)},
             {.color = 3, .picture = "GRE_TEXTSCROLL", .ink = Color::rgba(0x00, 0x1F, 0x00)},
             {.color = -1, .picture = "TEXTSCROLL", .ink = Color::rgba(0x16, 0x0C, 0x03)},
             {.color = 7, .picture = "TEXTSCROLL", .ink = Color::rgba(0x16, 0x0C, 0x03)},
         }}) {
        CAPTURE(coloured.color);
        view.help->color = coloured.color;
        const std::vector<Quad> drawn =
            dreamcast.drawn(view, &LegendsDreamcastHudPainter::drawHelp);
        REQUIRE(drawn.size() == quads.size());
        CHECK(Fixture::pictureOf(drawn.front().texture).name == coloured.picture);
        for (const Quad& glyph : std::span{drawn}.subspan(1)) {
            CHECK(glyph.color == coloured.ink);
        }
    }
    // The Dreamcast's own art: Dark Legacy's look is not asked.
    CHECK(dreamcast.art.record.count(kDarkLegacyHudStyle, test::FakeHudPart::Help) == 0);
}

TEST_CASE("a missing static set or font stops the Dreamcast look, and one that cannot be read "
          "says which",
          "[legends][dreamcast-hud]") {
    test::FakeRenderDevice device;
    test::FakeDarkLegacyArt art;
    SECTION("everything there") {
        const Fixture files("dreamcast-hud-load-whole");
        LegendsDreamcastHudPainter look;
        CHECK(look.load(device, files.root(), art, kWarriorAndWizard));
    }
    SECTION("the static set or a font missing") {
        for (const std::string_view name : LegendsDreamcastHudPainter::kRequiredFiles) {
            CAPTURE(name);
            const Fixture files("dreamcast-hud-load-missing");
            REQUIRE(std::filesystem::remove(files.root() / name));
            LegendsDreamcastHudPainter look;
            const u32 before = device.texturesCreated;
            CHECK_FALSE(look.load(device, files.root(), art, kWarriorAndWizard));
            CHECK(device.texturesCreated == before);
        }
    }
    SECTION("the static set or a font cut short") {
        // What cannot be read stops the look, naming the file, for the opener's one warning. A
        // font cut short has lost the empty record that ends its glyphs.
        for (const std::string_view name : LegendsDreamcastHudPainter::kRequiredFiles) {
            CAPTURE(name);
            const Fixture files("dreamcast-hud-load-short");
            files.truncate(name);
            LegendsDreamcastHudPainter look;
            try {
                (void)look.load(device, files.root(), art, kWarriorAndWizard);
                FAIL("a cut-short file was read");
            } catch (const FormatError& error) {
                CHECK(std::string_view{error.what()}.find(name) != std::string_view::npos);
            }
        }
    }
    SECTION("a static set without a picture the boxes draw") {
        for (const test::DreamcastPicture& left : Fixture::kStaticPictures) {
            if (left.name == Fixture::kUndrawn) {
                continue;
            }
            CAPTURE(left.name);
            const Fixture files("dreamcast-hud-load-incomplete");
            std::vector<test::DreamcastPicture> pictures;
            for (const test::DreamcastPicture& picture : Fixture::kStaticPictures) {
                if (picture.name != left.name) {
                    pictures.push_back(picture);
                }
            }
            files.writeBank(Fixture::kStaticFolder, pictures);
            LegendsDreamcastHudPainter look;
            try {
                (void)look.load(device, files.root(), art, kWarriorAndWizard);
                FAIL("a static set without a picture was taken");
            } catch (const FormatError& error) {
                CHECK(std::string_view{error.what()}.find(left.name) != std::string_view::npos);
            }
        }
    }
    SECTION("without the picture it does not draw") {
        const Fixture files("dreamcast-hud-load-undrawn");
        std::vector<test::DreamcastPicture> pictures;
        for (const test::DreamcastPicture& picture : Fixture::kStaticPictures) {
            if (picture.name != Fixture::kUndrawn) {
                pictures.push_back(picture);
            }
        }
        files.writeBank(Fixture::kStaticFolder, pictures);
        LegendsDreamcastHudPainter look;
        CHECK(look.load(device, files.root(), art, kWarriorAndWizard));
    }
}
