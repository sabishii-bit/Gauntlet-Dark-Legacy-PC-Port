#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/StringTable.h"
#include "engine/core/SpecialMembers.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"
#include "engine/math/Math.h"
#include "engine/render/Image.h"
#include "engine/render/RenderDevice.h"
#include "engine/render/RenderTypes.h"
#include "engine/ui/Canvas.h"
#include "engine/ui/TextPainter.h"

#include "FakeHudStyles.h"
#include "FakeRenderDevice.h"
#include "HudLookScene.h"
#include "LogCapture.h"
#include "RecordedDrawChecks.h"
#include "RecordedQuads.h"
#include "TestSupport.h"
#include "expansions/n64/formats/LegendsN64Font.h"
#include "expansions/n64/formats/LegendsN64Texture.h"
#include "expansions/n64/hud/LegendsN64Frame.h"
#include "expansions/n64/hud/LegendsN64HudLayout.h"
#include "expansions/n64/hud/LegendsN64HudPainter.h"
#include "expansions/n64/hud/LegendsN64Plaque.h"
#include "expansions/shared/app/LegendsHudStyles.h"
#include "expansions/shared/characters/LegendsProgress.h"
#include "expansions/shared/hud/LegendsHudStyleIds.h"
#include "game/characters/CharacterSave.h"
#include "game/characters/ClassData.h"
#include "game/characters/Inventory.h"
#include "game/characters/Party.h"
#include "game/config/GameConfig.h"
#include "game/hud/DarkLegacyHudArt.h"
#include "game/hud/HudPainter.h"
#include "game/hud/HudStyles.h"
#include "game/level/ExitPortals.h"
#include "game/level/LevelCatalog.h"
#include "game/scenes/PlayScene.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;
using Layout = LegendsN64HudLayout;
using test::FakeHudPart;
using test::HudPresses;

/** A plaque's transparency at rest (FUN_e0058620), as a seat long on its exit sees it. */
constexpr u8 kRestingTransparency = 0x1f;
constexpr u8 kOpaque = 255;
/** An RGBA texel's bytes, and where its alpha is among them. */
constexpr usize kTexelBytes = 4;
constexpr usize kAlphaByte = 3;
/** The scene's ticks from a seat stepping on an exit to its plaque resting: the plaque's seven
 * steps of the look's clock, two ticks a step at this rate, and some over. */
constexpr u32 kTicksToRest = 20;
/** Where the Legends looks' art is looked for: this checkout's own extracted data, and nowhere
 * else. */
LegendsHudRoots checkoutRoots() {
    return {.assetRoot = test::dataDirectory().parent_path(),
            .executableDirectory = {},
            .workingDirectory = {}};
}

/** Skips the case when this checkout holds no extraction look `id` can be drawn from. */
void needLook(std::string_view id) {
    if (!legendsHudFilesFound(id, checkoutRoots())) {
        SKIP(std::format("The {} look's Legends extraction is not installed", id));
    }
}

/** The Legends looks as the game offers and opens them, from this checkout's extractions. Each
 * id asked for is kept, and how many painters opened; each painter tells `record` of the calls
 * it takes. */
class CheckoutHudStyles final : public HudStyles {
public:
    std::span<const HudStyleOffer> offers() override {
        m_offers = legendsHudStyles(checkoutRoots());
        return m_offers;
    }
    std::unique_ptr<HudPainter> open(std::string_view id, RenderDevice& device,
                                     DarkLegacyHudArt& darkLegacy,
                                     std::span<const HudMember> party) override {
        auto painter = openLegendsHud(id, device, darkLegacy, party, checkoutRoots());
        opened.emplace_back(id);
        if (painter == nullptr) {
            return nullptr;
        }
        ++painters;
        return std::make_unique<test::WatchedHudPainter>(std::move(painter), record);
    }

    std::vector<std::string> opened; ///< each id asked for, in turn
    usize painters = 0;              ///< how many of them opened
    test::FakeHudRecord record;      ///< the calls the looks opened took, together

private:
    std::vector<HudStyleOffer> m_offers;
};

/** A level opened as the native digest tests open theirs, its HUD drawn in Dark Legacy's look
 * or in a Legends look opened from this checkout's own extractions, with bloom on so that its
 * overlay can be found. Skips without the game data. */
struct LookedScene : test::HudLookScene<CheckoutHudStyles> {
    explicit LookedScene(std::string_view stage, s32 seats = 2) : HudLookScene(stage, seats) {
        config.display.bloom = true;
    }
};

/** The warnings of `warnings` about a HUD look or the art it reads. */
std::vector<std::string> hudWarnings(std::span<const std::string> warnings) {
    std::vector<std::string> found;
    std::ranges::copy_if(warnings, std::back_inserter(found), [](const std::string& warning) {
        return warning.find("HUD") != std::string::npos;
    });
    return found;
}

/** The canvas the scene draws its HUD on in those frames. */
Mat4 hudCanvas() {
    return makeVirtualScreenTransform(
        makeScreenProjection(LookedScene::kFrameWidth, LookedScene::kFrameHeight),
        static_cast<f32>(GameConfig{}.display.virtualWidth),
        static_cast<f32>(GameConfig{}.display.virtualHeight), LookedScene::kFrameWidth,
        LookedScene::kFrameHeight);
}

/** Checks `actual` is `expected`, but for float rounding. */
void checkTransform(const Mat4& actual, const Mat4& expected) {
    constexpr f64 kMargin = 1e-5;
    for (s32 column = 0; column < 4; ++column) {
        for (s32 row = 0; row < 4; ++row) {
            CAPTURE(column, row);
            CHECK(actual[column][row] == Approx(expected[column][row]).margin(kMargin));
        }
    }
}

/** `text` in capitals, as the port's plaques write a level's title. */
std::string capitals(std::string_view text) {
    std::string upper(text);
    for (char& c : upper) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return upper;
}

/** One of the N64 extraction's fonts as the N64 look writes with it, its sheet on a device of
 * the case's own. */
class N64Lettering {
public:
    N64Lettering(RenderDevice& device, const std::filesystem::path& file)
        : m_font(formats::LegendsN64Font::parse(readFile(file))) {
        m_sheet = device.createTexture(
            TextureDesc{.width = m_font.atlas.width, .height = m_font.atlas.height},
            m_font.atlas.pixels);
        m_text.setFont(&m_font.font, m_sheet.get());
    }
    ~N64Lettering() = default;
    GDL_NON_COPYABLE_NON_MOVABLE(N64Lettering);

    const Texture* sheet() const { return m_sheet.get(); }
    /** How wide `words` are written. */
    f32 width(std::string_view words) const { return static_cast<f32>(m_text.measure(words)); }
    /** The vertices `words` are written with from their line's own origin in `color`, as a
     * plaque writes each of its lines through a frame of its own. */
    std::vector<ImmediateVertex> line(test::FakeRenderDevice& device, std::string_view words,
                                      Color color) const {
        device.draws.clear();
        Canvas canvas;
        canvas.begin(device, Mat4{1.0f});
        m_text.draw(canvas, 0, 0, words, TextStyle{.color = color});
        canvas.end();
        REQUIRE(device.draws.size() == 1);
        return device.draws.front().vertices;
    }

private:
    formats::LegendsN64Font m_font;
    std::unique_ptr<Texture> m_sheet;
    TextPainter m_text;
};

/** Where a plaque's line `width` wide, written from row `top` of panel `slot`, stands: centred on
 * the panel through a frame of its own, shrunk to the plaque's text width when wider. */
Mat4 plaqueLine(usize slot, f32 top, f32 width) {
    const f32 fit = std::min(1.0f, static_cast<f32>(Layout::kPlaqueTextWidth) / width);
    const f32 centre =
        static_cast<f32>(Layout::kSlotX.at(slot)) + static_cast<f32>(Layout::kPanelWidth) / 2;
    return hudCanvas() * LegendsN64Frame::transform() *
           glm::scale(
               glm::translate(Mat4{1.0f}, Vec3{centre - width * fit / 2,
                                               static_cast<f32>(Layout::kStripTop) + top, 0.0f}),
               Vec3{fit, fit, 1.0f});
}

/** The draws of `device`, in the order drawn, that write the line `words`, vertex for vertex. */
std::vector<const test::RecordedDraw*> linesWriting(const test::FakeRenderDevice& device,
                                                    std::span<const ImmediateVertex> words) {
    std::vector<const test::RecordedDraw*> found;
    for (const test::RecordedDraw& draw : device.draws) {
        if (std::ranges::equal(draw.vertices, words)) {
            found.push_back(&draw);
        }
    }
    return found;
}

/** The area `quads` cover together. */
Rect covered(std::span<const test::Quad> quads) {
    REQUIRE_FALSE(quads.empty());
    f32 left = quads.front().area.x;
    f32 top = quads.front().area.y;
    f32 right = quads.front().area.right();
    f32 bottom = quads.front().area.bottom();
    for (const test::Quad& quad : quads) {
        left = std::min(left, quad.area.x);
        top = std::min(top, quad.area.y);
        right = std::max(right, quad.area.right());
        bottom = std::max(bottom, quad.area.bottom());
    }
    return {left, top, right - left, bottom - top};
}

} // namespace

TEST_CASE("the N64 and Dreamcast looks drawn from the player's own art change no byte of the "
          "digests",
          "[legends][hud-style][assets]") {
    const std::string_view look = GENERATE(kLegendsN64HudStyle, kLegendsDreamcastHudStyle);
    const s32 seats = GENERATE(2, 4);
    CAPTURE(look, seats);
    needLook(look);
    auto darkLegacy = std::make_unique<LookedScene>("G2", seats);
    auto looked = std::make_unique<LookedScene>("G2", seats);
    for (LookedScene* fixture : {darkLegacy.get(), looked.get()}) {
        fixture->party[0].save.progress().inventory.powerups[0] = test::kSelectorPowerup;
    }
    darkLegacy->open();

    // The look opens on the player's art and draws every frame, never giving way to Dark
    // Legacy's: no warning of a look's is logged from its opening to the last tick.
    const std::vector<std::string> warnings = test::warningsWhile([&] {
        looked->open(look);
        REQUIRE(looked->styles.opened == std::vector{std::string{look}});
        REQUIRE(looked->styles.painters == 1);
        // It took its own art as it opened.
        CHECK(looked->device.texturesCreated > darkLegacy->device.texturesCreated);
        CHECK(darkLegacy->digest() == looked->digest());

        for (const u32 anchor : {HudPresses::kFirstAnchor, HudPresses::kLastTick}) {
            while (looked->tick < anchor) {
                const PlayScene::Inputs inputs = test::hudInputsAt(looked->tick + 1);
                darkLegacy->step(inputs);
                looked->step(inputs);
                // Both draw every tick: the one Dark Legacy's boxes, the other the look.
                darkLegacy->render(0.5f);
                looked->render(0.5f);
                CAPTURE(looked->tick);
                REQUIRE(darkLegacy->digest() == looked->digest());
            }
            PlayScene::LocalSnapshot plain;
            PlayScene::LocalSnapshot shown;
            REQUIRE(darkLegacy->scene.saveLocal(plain));
            REQUIRE(looked->scene.saveLocal(shown));
            CHECK(darkLegacy->digest(&plain) == looked->digest(&shown));
        }
    });
    CHECK(hudWarnings(warnings) == std::vector<std::string>{});
    // The look prepared every frame; the closed selectors' presses turned its pages, and the
    // first seat's right, while its selector stood open, steered it.
    const test::FakeHudRecord& record = looked->styles.record;
    CHECK(record.count(look, FakeHudPart::Prepare) == HudPresses::kLastTick);
    CHECK(record.count(look, FakeHudPart::Browse) > 0);
    CHECK(record.count(look, FakeHudPart::Steer) == 1);
    // What the last frame drew over the view is the look's, not Dark Legacy's.
    const std::vector<ImmediateVertex> lookedOverlay = looked->overlay();
    const std::vector<ImmediateVertex> plainOverlay = darkLegacy->overlay();
    const bool alike = lookedOverlay == plainOverlay;
    CHECK_FALSE(alike);
    // No other look was asked for on the way.
    CHECK(looked->styles.opened == std::vector{std::string{look}});
}

TEST_CASE("seat 1 standing on the tower's G1 portal sees that portal's plaque in its own panel",
          "[legends][hud-style][assets]") {
    needLook(kLegendsN64HudStyle);
    const auto n64 = legendsHudRoot(LegendsPlatform::N64, checkoutRoots());
    REQUIRE(n64.has_value());
    // Two players, the second seat first in the party: an archer in red, then a warrior in
    // green, neither in its own seat's colour.
    constexpr s32 kPlaqueSeat = 1;
    constexpr s32 kRed = 2;
    constexpr s32 kGreen = 3;
    auto fixture = std::make_unique<LookedScene>("L1");
    std::ranges::reverse(fixture->party);
    fixture->party[0].save.character = kArcherClass;
    fixture->party[0].save.color = kRed;
    fixture->party[1].save.color = kGreen;
    const std::vector<std::string> opening =
        test::warningsWhile([&] { fixture->open(kLegendsN64HudStyle); });
    CHECK(hudWarnings(opening) == std::vector<std::string>{});
    REQUIRE(fixture->styles.painters == 1);
    fixture->settle();
    PlayScene& scene = fixture->scene;
    REQUIRE(scene.participants().front().actor.player() == kPlaqueSeat);
    // A frame before anyone moves starts the look's clock.
    fixture->render();

    const ExitPortals& exits = scene.portals();
    usize g1 = 0;
    while (g1 < exits.size() &&
           (!exits.portal(g1).destination || exits.portal(g1).destination->name != "G1")) {
        ++g1;
    }
    REQUIRE(g1 < exits.size());
    const LevelRef& destination = *exits.portal(g1).destination;
    const auto realm = darkLegacyRealmNameText(destination.realmId);
    REQUIRE(realm.has_value());
    REQUIRE(fixture->strings.has(*realm));
    const std::string_view realmName = fixture->strings.get(*realm);
    const std::string title = capitals(destination.title);
    REQUIRE_FALSE(title.empty());

    // The seat steps onto the portal to G1, the other staying where it came in, and stands there
    // until its plaque has come to rest.
    REQUIRE(scene.actor(kPlaqueSeat) != nullptr);
    REQUIRE(scene.noclipMove(kPlaqueSeat,
                             exits.portal(g1).position - scene.actor(kPlaqueSeat)->position(),
                             std::nullopt));
    for (u32 tick = 0; tick < kTicksToRest; ++tick) {
        fixture->step({});
    }
    fixture->render();

    const auto opacity = static_cast<u8>(kOpaque - kRestingTransparency);
    const Rect slot{static_cast<f32>(Layout::kSlotX.at(kPlaqueSeat)),
                    static_cast<f32>(Layout::kStripTop), static_cast<f32>(Layout::kPanelWidth),
                    static_cast<f32>(Layout::kPanelHeight)};
    const Mat4 native = hudCanvas() * LegendsN64Frame::transform();
    // The port's plaque over that seat's panel alone: the panel's background lit in the seat's
    // costume colour and its frame, as the N64 draws its own, at rest.
    std::vector<formats::LegendsN64Texture> hud =
        formats::decodeLegendsN64Textures(readFile(*n64 / LegendsN64HudPainter::kHudBank));
    const auto pictureOf = [&](std::string_view name) -> Image& {
        const auto found = std::ranges::find(hud, name, &formats::LegendsN64Texture::name);
        REQUIRE(found != hud.end());
        return found->image;
    };
    Image& background = pictureOf("COLOR");
    // The look draws it opaque, as the N64 does.
    for (usize alpha = kAlphaByte; alpha < background.pixels.size(); alpha += kTexelBytes) {
        background.pixels[alpha] = kOpaque;
    }
    const Image& border = pictureOf("BORDER");
    const std::vector<test::Quad> quads = test::quadsOf(fixture->device);
    const auto plaqueQuads = [&](const Image& picture, Color color) {
        std::vector<test::Quad> found;
        for (const test::Quad& quad : quads) {
            if (quad.color.a == opacity && test::showsPixels(quad, picture)) {
                CHECK(quad.color == color);
                checkTransform(quad.transform, native);
                found.push_back(quad);
            }
        }
        return found;
    };
    test::checkRect(covered(plaqueQuads(background, Layout::kBright.at(kRed).withAlpha(opacity))),
                    slot);
    test::checkRect(covered(plaqueQuads(border, Color::white().withAlpha(opacity))), slot);

    // On it the realm's name in gold, then G1's title in white, each centred on that panel.
    test::FakeRenderDevice letters;
    const N64Lettering small(letters, *n64 / LegendsN64HudPainter::kSmallFontFile);
    const N64Lettering big(letters, *n64 / LegendsN64HudPainter::kTitleFontFile);
    const std::vector<ImmediateVertex> realmLine =
        small.line(letters, realmName, Layout::kPlaqueRealmInk.withAlpha(opacity));
    const std::vector<ImmediateVertex> titleLine =
        big.line(letters, title, Color::white().withAlpha(opacity));
    CAPTURE(realmName, title);
    const auto realms = linesWriting(fixture->device, realmLine);
    REQUIRE(realms.size() == 1);
    test::checkTextureImage(realms.front()->texture, small.sheet());
    checkTransform(
        realms.front()->transform,
        plaqueLine(kPlaqueSeat, static_cast<f32>(Layout::kPlaqueRealmTop), small.width(realmName)));
    const auto titles = linesWriting(fixture->device, titleLine);
    REQUIRE(titles.size() == 1);
    test::checkTextureImage(titles.front()->texture, big.sheet());
    checkTransform(
        titles.front()->transform,
        plaqueLine(kPlaqueSeat, static_cast<f32>(Layout::kPlaqueTitleTop), big.width(title)));
    // The realm's line is written first.
    CHECK(realms.front() < titles.front());
}
