#include <array>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/assets/BitmapFont.h"
#include "engine/io/File.h"
#include "engine/ui/TextPainter.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/hud/LevelMessages.h"
#include "game/scenes/PlayScene.h"
#include "game/tower/SumnerVisit.h"

namespace {
using namespace gdl;
using namespace gdl::game;

struct BurnArt {
    Image scroll = Image::filled(4, 4, Color::white());
    Image mask = Image::filled(2, 2, Color::white());
    test::FakeTexture ring{2, 2};

    ScrollBoxArt scrollArt() const {
        ScrollBoxArt result;
        result.backdropImage = &scroll;
        result.burnMasks = {&mask};
        result.burnRing = {&ring};
        return result;
    }
    HintMenuArt hintArt() const {
        HintMenuArt result;
        result.scroll = &scroll;
        result.burnMasks = {&mask};
        result.burnRing = {&ring};
        return result;
    }
};

std::filesystem::path sampleRoot(bool font, bool burn, bool hints) {
    const auto root =
        test::scratchDirectory(std::format("native-ready-{}-{}-{}", font, burn, hints));
    std::filesystem::create_directories(root / "STATIC");
    std::filesystem::create_directories(root / "fonts");
    std::filesystem::create_directories(root / "text");
    writeFile(root / "STATIC/tile.png", test::kTinyPng);
    std::vector<std::string_view> names{"SCROLL_A", "FONT32_GLOW"};
    if (font) {
        names.emplace_back("FONT32");
    }
    if (burn) {
        names.insert(names.end(), {"GREENCIRCTRANS", "GREENCIRCTRANS+1", "GREENCIRCTRANSM",
                                   "GREENCIRCTRANSM+1"});
    }
    std::string manifest = R"({"bitmaps":[)";
    for (usize index = 0; index < names.size(); ++index) {
        if (index != 0) {
            manifest += ',';
        }
        manifest += std::format(R"({{"index":{},"name":"{}","file":"tile.png",
                                    "width":2,"height":2}})",
                                index, names[index]);
    }
    writeTextFile(root / "STATIC/textures.json", manifest + "]}");
    writeTextFile(root / "fonts/font32.json", R"({"height":10,"glyphs":[
        {"code":65,"width":6,"x":0,"y":0}]})");
    writeTextFile(root / "text/scroll_e.json", R"({"fonts":["font32"],"messages":[
        {"name":"WELCOME","font":0,"scale":1,"lines":["A"]}],"lists":[]})");
    if (hints) {
        writeTextFile(root / "text/hints_e.json", R"({"fonts":["font32"],"messages":[
            {"name":"GENERAL","font":0,"scale":1,"lines":["A"]}],
            "lists":[{"name":"GENERAL_HINTS","messages":[0]}]})");
    }
    return root;
}
} // namespace

TEST_CASE("native burn readiness accepts short lists without allocating or changing hold timing",
          "[native-presentation-ready]") {
    const BurnArt art;
    ScrollBox box;
    HintMenu hints;
    CHECK_FALSE(box.burnReady());
    CHECK_FALSE(hints.burnReady());
    box.setArt(art.scrollArt());
    hints.setArt(art.hintArt());
    REQUIRE(box.burnReady());
    REQUIRE(hints.burnReady());
    test::FakeRenderDevice device;
    const auto font = BitmapFont::fromGlyphs(10, 4, {{'A', 6, 0, 0}});
    test::FakeTexture sheet{8, 8};
    TextPainter text;
    text.setFont(&font, &sheet);
    box.setText(&text);
    const auto uploads = device.texturesCreated;
    CHECK(box.burnReady());
    CHECK(hints.burnReady());
    CHECK(device.texturesCreated == uploads);
    REQUIRE(box.open(device, {"A"}, 1, "A"));
    box.step(ScrollBox::kHoldTicks, 0);
    box.step(1, 1);
    REQUIRE(box.burning());
    box.step(BurnDialogueScroll::kFrameCount * BurnDialogueScroll::kTicksPerFrame - 1, 0);
    REQUIRE(box.burning());
    box.step(1, 0);
    CHECK_FALSE(box.active());
    CHECK(box.burnReady()); // Readiness is a binding property, not interaction state.
}

TEST_CASE("native burn readiness rejects missing malformed and inconsistent borrowed artwork",
          "[native-presentation-ready]") {
    BurnArt art;
    auto scroll = art.scrollArt();
    auto hint = art.hintArt();
    const Image malformed = Image::filled(3, 2, Color::white());
    test::FakeTexture emptyTexture{0, 0};
    SECTION("missing scroll") {
        scroll.backdropImage = nullptr;
        hint.scroll = nullptr;
    }
    SECTION("empty scroll pixels") {
        art.scroll.pixels.clear();
    }
    SECTION("truncated scroll pixels") {
        art.scroll.pixels.pop_back();
    }
    SECTION("no masks") {
        scroll.burnMasks.clear();
        hint.burnMasks.clear();
    }
    SECTION("null first mask") {
        scroll.burnMasks[0] = nullptr;
        hint.burnMasks[0] = nullptr;
    }
    SECTION("inconsistent mask dimensions") {
        scroll.burnMasks.push_back(&malformed);
        hint.burnMasks.push_back(&malformed);
    }
    SECTION("empty mask pixels") {
        art.mask.pixels.clear();
    }
    SECTION("no rings") {
        scroll.burnRing.clear();
        hint.burnRing.clear();
    }
    SECTION("null ring") {
        scroll.burnRing[0] = nullptr;
        hint.burnRing[0] = nullptr;
    }
    SECTION("empty ring texture") {
        scroll.burnRing[0] = &emptyTexture;
        hint.burnRing[0] = &emptyTexture;
    }
    ScrollBox box;
    HintMenu menu;
    box.setArt(scroll);
    menu.setArt(hint);
    CHECK_FALSE(box.burnReady());
    CHECK_FALSE(menu.burnReady());
}

TEST_CASE("native message and hint readiness require actual text font and burn resources",
          "[native-presentation-ready]") {
    const bool font = GENERATE(false, true);
    const bool burn = GENERATE(false, true);
    const bool hints = GENERATE(false, true);
    CAPTURE(font, burn, hints);
    const auto root = sampleRoot(font, burn, hints);
    test::FakeRenderDevice device;
    TextureSet textures;
    ItemArchive powerups;
    LevelMessages messages;
    SumnerVisit visit;
    messages.load(device, textures, root, nullptr);
    visit.load(device, textures, powerups, root, nullptr);
    REQUIRE(messages.strings().loaded());
    CHECK(messages.text().ready() == font);
    CHECK(messages.nativeReady() == (font && burn));
    CHECK(visit.nativeReady(messages.text()) == (font && burn && hints));
    CHECK_FALSE(visit.nativeReady(TextPainter{}));
    const auto uploads = device.texturesCreated;
    const auto changes = device.textureUpdates;
    CHECK(messages.nativeReady() == (font && burn));
    CHECK(visit.nativeReady(messages.text()) == (font && burn && hints));
    CHECK(device.texturesCreated == uploads);
    CHECK(device.textureUpdates == changes);
    CHECK(device.draws.empty());
    // The native-only guard does not change offline optional-art semantics.
    REQUIRE(messages.open(device, "WELCOME", nullptr) == font);
    if (font) {
        messages.step(ScrollBox::kHoldTicks, 0);
        messages.step(1, 1);
        CHECK(messages.active() == burn);
    }
    visit.clear();
    CHECK_FALSE(visit.nativeReady(messages.text()));
    messages.clear();
    CHECK_FALSE(messages.nativeReady());
}

TEST_CASE("native scene readiness includes Sumner only for the tower and revokes on close",
          "[native-presentation-ready][assets]") {
    const auto name = GENERATE(std::string_view{"G1"}, std::string_view{"L1"});
    const bool pojo = GENERATE(false, true);
    CAPTURE(name, pojo);
    struct Owners {
        test::FakeRenderDevice device;
        GameConfig config;
        LevelCatalog catalog;
        LevelWorld world;
        PlayScene scene;
    };
    GameServices context;
    auto owners = std::make_unique<Owners>();
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    REQUIRE(owners->catalog.load(root));
    const auto level =
        name == "L1" ? std::optional{LevelRef::tower()} : owners->catalog.byName(name);
    REQUIRE(level);
    REQUIRE(owners->world.load(owners->device, root, *level));
    CHECK_FALSE(owners->scene.nativePresentationReady());
    context.config = &owners->config;
    context.levels = &owners->catalog;
    context.unpackedRoot = root;
    CharacterSave save;
    // spawnParty reapplies the name-derived form; an arbitrary inventory bit
    // on a non-EGG911 character is correctly cleared by restoreNameForm.
    save.name = pojo ? "EGG911" : "READY";
    const std::array<PartyMember, 1> party{PartyMember{0, save}};
    PlayOptions options;
    options.welcome = false;
    REQUIRE(owners->scene.open(owners->device, context, owners->world, party, options));
    const auto* player = owners->scene.runtime(0);
    REQUIRE(player != nullptr);
    REQUIRE(player->actor.save().progress().inventory.permanentPojo == pojo);
    REQUIRE(player->figure != nullptr);
    REQUIRE(player->figure->animator().bound());
    REQUIRE(player->figure->pojoActive() == pojo);
    const auto uploads = owners->device.texturesCreated;
    REQUIRE(owners->scene.nativePresentationReady());
    CHECK(owners->device.texturesCreated == uploads);
    CHECK(owners->device.draws.empty());
    // This fixture owns the menu; drop its borrowed art to model an optional
    // tower-load failure, without modifying message assets or gameplay state.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    const_cast<HintMenu&>(owners->scene.hints()).setArt({});
    CHECK(owners->scene.nativePresentationReady() == (name != "L1"));
    owners->scene.close();
    CHECK_FALSE(owners->scene.nativePresentationReady());
}

TEST_CASE("native scene readiness rejects failed figure and action bindings for any joined seat",
          "[native-presentation-ready][assets]") {
    struct Owners {
        test::FakeRenderDevice device;
        GameConfig config;
        LevelCatalog catalog;
        LevelWorld world;
        PlayScene scene;
    };
    GameServices context;
    auto owners = std::make_unique<Owners>();
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    REQUIRE(owners->catalog.load(root));
    const auto level = owners->catalog.byName("G1");
    REQUIRE(level);
    REQUIRE(owners->world.load(owners->device, root, *level));
    context.config = &owners->config;
    context.levels = &owners->catalog;
    context.unpackedRoot = root;
    CharacterSave save;
    save.name = "READY";
    const std::array<PartyMember, 2> party{PartyMember{1, save}, PartyMember{3, save}};
    PlayOptions options;
    options.welcome = false;
    REQUIRE(owners->scene.open(owners->device, context, owners->world, party, options));
    REQUIRE(owners->scene.nativePresentationReady());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
    auto* player = const_cast<PlayerRuntime*>(owners->scene.runtime(3));
    REQUIRE(player != nullptr);
    REQUIRE(player->figure != nullptr);
    REQUIRE(player->figure->animator().bound());

    SECTION("body load failure returns no figure") {
        const auto missing = test::scratchDirectory("native-ready-player-load") / "absent";
        REQUIRE_FALSE(std::filesystem::exists(missing));
        // Model the nullable result which PlayScene::open intentionally accepts
        // for offline play, without changing the shared native asset files.
        auto failed = PlayerFigure::load(owners->device, missing, player->actor.save());
        REQUIRE(failed == nullptr);
        player->figure = std::move(failed);
    }
    SECTION("action archive lacks the required READY sequence") {
        const TreeInfo missingReady;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): mutable test fixture
        auto& animator = const_cast<PlayerAnimator&>(player->figure->animator());
        REQUIRE_FALSE(animator.bind(missingReady));
        REQUIRE_FALSE(animator.bound());
    }

    const auto* healthy = owners->scene.runtime(1);
    REQUIRE(healthy != nullptr);
    REQUIRE(healthy->figure != nullptr);
    REQUIRE(healthy->figure->animator().bound());
    const auto uploads = owners->device.texturesCreated;
    const auto changes = owners->device.textureUpdates;
    CHECK_FALSE(owners->scene.nativePresentationReady());
    CHECK_FALSE(owners->scene.nativePresentationReady());
    CHECK(owners->device.texturesCreated == uploads);
    CHECK(owners->device.textureUpdates == changes);
    CHECK(owners->device.draws.empty());
}
