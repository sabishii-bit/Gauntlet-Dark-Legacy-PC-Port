#include <array>
#include <filesystem>
#include <string_view>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/NativeStateDigest.h"
#include "engine/world/SampleLevel.h"

#include "FakeRenderDevice.h"
#include "NativeStateSupport.h"
#include "TestSupport.h"
#include "game/bosses/BossSequence.h"
#include "game/level/ExitPortals.h"
#include "game/level/LevelTransporters.h"
#include "game/level/NativeStateResources.h"
#include "game/party/PortalDeparture.h"
#include "game/tower/LevelArrivalPresentation.h"
#include "game/tower/SumnerFigure.h"
#include "game/tower/SumnerVisit.h"
#include "game/tower/TowerPromotion.h"
#include "game/tower/TowerRelics.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Digest = NativeStateDigest::Digest;
constexpr f32 kStep = 1.0f / 60;

template <class T> Digest native(const T& value, const NativeStateResources& resources) {
    NativeStateDigest sink(resources.resources());
    if constexpr (requires { value.appendNativeDigest(sink); }) {
        REQUIRE(value.appendNativeDigest(sink));
    } else {
        sink.value(value);
    }
    const auto result = sink.checkedFinish();
    REQUIRE(result);
    return *result;
}

template <class T>
Digest captured(const T& value, const typename T::LocalSnapshot& saved,
                const NativeStateResources& resources) {
    NativeStateDigest sink(resources.resources());
    REQUIRE(value.appendLocalDigest(sink, saved));
    const auto result = sink.checkedFinish();
    REQUIRE(result);
    return *result;
}

struct Travel {
    test::FakeRenderDevice device;
    ItemArchive items;
    WorldLayout layout;
    WorldCollision floor;
    LevelCatalog catalog;
    LevelTransporters pads;
    ExitPortals exits;
    LevelArrivalPresentation arrival;
    PortalDeparture departure;
    NativeStateResources resources;

    explicit Travel(std::string_view name) {
        const auto path = test::sampleLevel(name);
        std::filesystem::create_directories(path / "wdata");
        writeTextFile(path / "wdata/TOWN.json", R"({"realm":7,"prefix":"levelG",
          "levels":[{"name":"G1","title":"Town"}]})");
        writeTextFile(path / "wdata/SECRET.json", R"({"realm":12,"prefix":"levelS",
          "levels":[{"name":"S1","title":"Secret"}]})");
        writeTextFile(path / "world.json", R"({
          "objects":[{"name":"FLOOR","position":[0,0,0],"flags":4102}],
          "itemInfos":[{"type":9,"name":"EXIT_PORTAL","radius":3},
            {"type":9,"subtype":50,"name":"SECRET_ICON","radius":1},
            {"type":11,"name":"TRANS","radius":2,"height":2}],
          "itemInstances":[
            {"info":0,"position":[0,0,0],"params":[0,0,0,0,103,49,0,0]},
            {"info":1,"position":[20,0,0],"params":[0,0,0,0,83,49,0,0]},
            {"info":2,"position":[-10,0,0],"params":[1,0,0,0,2,0,0,0]},
            {"info":2,"position":[10,0,0],"params":[2,0,0,0,1,0,0,0]}]})");
        writeTextFile(path / "animations.json", R"({
          "textureAnimations":[{"name":"STONE","texture":0,"source":1,"frames":2,"rate":1}],
          "trees":[
            {"name":"EXIT_PORTAL","nodes":[{"name":"BODY","object":"WALL","position":[0,0,0]}],
             "sequences":[{"name":"IDLE","frames":4,"frameRate":30},
              {"name":"READY","frames":4,"frameRate":30},
              {"name":"ACTIVE1","frames":4,"frameRate":30},
              {"name":"ACTIVE2","frames":4,"frameRate":30},
              {"name":"ACTIVE3","frames":4,"frameRate":30}]},
            {"name":"SECRET_ICON","nodes":[{"name":"BODY","object":"WALL","position":[0,0,0]}],
             "sequences":[{"name":"IDLE","frames":9,"frameRate":30}]},
            {"name":"TRANS","nodes":[{"name":"BODY","object":"WALL","position":[0,0,0]}],
             "sequences":[{"name":"ACTIVE","frames":8,"frameRate":30}]},
            {"name":"STARTFX","nodes":[{"name":"BODY","object":"WALL","position":[0,0,0]}],
             "sequences":[{"name":"START","frames":8,"frameRate":30}]},
            {"name":"WIZARD","nodes":[{"name":"BODY","object":"WALL","position":[0,0,0]}],
             "sequences":[{"name":"READY","frames":8,"frameRate":30}]}]})");
        REQUIRE(items.load(path));
        REQUIRE(layout.load(path));
        REQUIRE(catalog.load(path));
        floor.build(
            {{.vertices = {Vec3{-40, 0, -40}, Vec3{40, 0, 40}, Vec3{40, 0, -40}}, .object = 0},
             {.vertices = {Vec3{-40, 0, -40}, Vec3{-40, 0, 40}, Vec3{40, 0, 40}}, .object = 0}});
        floor.setMovingObjects(std::array<s32, 1>{0});
        REQUIRE(exits.bind(device, layout, AssetScope(items), catalog, &floor));
        pads.bind(device, layout, AssetScope(items), 2, &floor);
        REQUIRE(exits.size() == 2);
        REQUIRE(pads.size() == 2);
        const std::array party{Vec3{-2, 0, 10}, Vec3{2, 0, 10}};
        arrival.begin(device, items, party, WorldCamera{});
        REQUIRE(arrival.effectCount() == 2);
        departure.loadSkin(device, items.textures);
        refresh();
    }

    void refresh() {
        const auto created = device.texturesCreated;
        REQUIRE(resources.registerBuiltinTexture(device.whiteTexture(), "opaque-white"));
        REQUIRE(resources.registerArchive(items, "items/travel-fixture"));
        REQUIRE(resources.registerLayout(layout, "travel-fixture"));
        REQUIRE(resources.registerCollision(floor, "travel-fixture"));
        REQUIRE(exits.registerNativeResources(resources));
        CHECK(device.texturesCreated == created);
    }

    struct Saved {
        WorldCollision::LocalSnapshot floor;
        ExitPortals::LocalSnapshot exits;
        LevelTransporters::LocalSnapshot pads;
        LevelArrivalPresentation::LocalSnapshot arrival;
        PortalDeparture::LocalSnapshot departure;
    };
    void save(Saved& state) const {
        floor.saveLocal(state.floor);
        exits.saveLocal(state.exits);
        pads.saveLocal(state.pads);
        arrival.saveLocal(state.arrival);
        departure.saveLocal(state.departure);
    }
    void restore(const Saved& state) {
        REQUIRE(floor.restoreLocal(state.floor));
        REQUIRE(exits.restoreLocal(state.exits));
        REQUIRE(pads.restoreLocal(state.pads));
        REQUIRE(arrival.restoreLocal(state.arrival));
        REQUIRE(departure.restoreLocal(state.departure));
    }
    auto digest() const {
        return std::array{native(exits, resources), native(pads, resources),
                          native(arrival, resources), native(departure, resources)};
    }
    auto digest(const Saved& saved) const {
        return std::array{captured(exits, saved.exits, resources),
                          captured(pads, saved.pads, resources),
                          captured(arrival, saved.arrival, resources),
                          captured(departure, saved.departure, resources)};
    }
    auto step(s32 tick, bool wrong) {
        const f32 height = wrong ? -5 : static_cast<f32>(tick % 24) * 0.05f;
        floor.setObjectTransform(0, glm::translate(Mat4{1}, Vec3{0, height, 0}));
        pads.syncFloors(floor);
        pads.capturePresentation();
        pads.animate(wrong ? 0.3f : kStep);
        arrival.capturePresentation();
        arrival.animate(wrong ? 0.3f : kStep);
        arrival.advance(wrong ? 9 : 1, wrong, {0, 10, -10}, {0, 0, 10});
        const std::array visitors{
            PortalVisitor{{wrong ? 20.0f : 0.0f, height, 0}, 0.75f, 3, true},
            PortalVisitor{{tick % 100 < 70 ? 10.0f : 0.0f, height, 0}, 0.75f, 1, true}};
        exits.setAlpha("g1", wrong ? 0.1f : 0.8f);
        if (wrong) {
            exits.consume(1);
        }
        const auto leave = exits.update(wrong ? 5 : 1, wrong ? 0.3f : kStep, visitors);
        if (wrong || tick == 100) {
            departure.begin(device, items.textures);
        }
        departure.update(wrong ? 7 : 1);
        return std::tuple{leave, exits.takeWaiting(), arrival.takeTitleLanded()};
    }
};

TEST_CASE("travel native hashes survive independent bindings and one to eight tick corrections",
          "[native-state-digest][scene-travel-native-digest][local-rewind]") {
    Travel correct("travel-digest-correct");
    Travel reference("travel-digest-reference");
    REQUIRE(correct.arrival.effectPresentation(0)->tree !=
            reference.arrival.effectPresentation(0)->tree);
    REQUIRE(correct.digest() == reference.digest());
    s32 tick = 0;
    bool sawWaiting = false;
    for (s32 window = 0; window < 48; ++window) {
        const s32 distance = window % 8 + 1;
        CAPTURE(window, distance, tick);
        Travel::Saved saved;
        correct.save(saved);
        const auto initial = correct.digest();
        REQUIRE(correct.digest(saved) == initial);
        for (s32 offset = 0; offset < distance; ++offset) {
            correct.step(tick + offset, true);
        }
        CHECK(correct.digest() != initial);
        CHECK(correct.digest(saved) == initial);
        correct.restore(saved);
        CHECK(correct.digest() == initial);
        for (s32 offset = 0; offset < distance; ++offset) {
            const auto expected = reference.step(tick + offset, false);
            const auto actual = correct.step(tick + offset, false);
            CHECK((actual == expected));
            sawWaiting = sawWaiting || !std::get<1>(actual).empty();
        }
        REQUIRE(correct.digest() == reference.digest());
        const auto stable = correct.digest();
        for (const f32 alpha : {-1.0f, 0.0f, 0.37f, 1.0f}) {
            correct.device.draws.clear();
            correct.arrival.drawEffects(correct.device, Mat4{1}, {}, alpha);
            correct.pads.draw(correct.device, Mat4{1}, {}, alpha);
            correct.exits.draw(correct.device, Mat4{1}, {});
            CHECK(correct.digest() == stable);
        }
        tick += distance;
    }
    CHECK(sawWaiting);
    Travel::Saved saved;
    correct.save(saved);
    NativeStateDigest foreign(reference.resources.resources());
    CHECK_FALSE(reference.exits.appendLocalDigest(foreign, saved.exits));
    CHECK_FALSE(foreign.checkedFinish());
    correct.exits.clear();
    NativeStateDigest stale(correct.resources.resources());
    CHECK_FALSE(correct.exits.appendLocalDigest(stale, saved.exits));
    CHECK_FALSE(stale.checkedFinish());
    const NativeStateResources empty;
    NativeStateDigest unknown(empty.resources());
    CHECK_FALSE(reference.arrival.appendNativeDigest(unknown));
    CHECK_FALSE(unknown.checkedFinish());
}

TEST_CASE("inactive entry camera and queued promotion clocks remain in the native digest",
          "[native-state-digest][scene-travel-native-digest]") {
    const NativeStateResources resources;
    StartCamera camera;
    camera.start(WorldCamera{Vec3{0, 20, -30}}, Vec3{0});
    const auto savedCamera = camera;
    const auto initialCamera = native(camera, resources);
    camera.update(1, false, {0, 10, -10}, {0, 0, 10});
    CHECK(native(camera, resources) != initialCamera);
    camera = savedCamera;
    CHECK(native(camera, resources) == initialCamera);
    camera.stop();
    const StartCamera neverStarted;
    REQUIRE_FALSE(camera.active());
    REQUIRE_FALSE(neverStarted.active());
    CHECK(native(camera, resources) != native(neverStarted, resources));

    std::array<PlayerRuntime, 1> party;
    CharacterSave save;
    save.progress().experience = levelExperience(30);
    save.progress().promotedLevel = 29;
    party[0].actor.spawn(0, save, nullptr, Vec3{0}, 0);
    const MessageTable strings;
    TowerPromotion promotion;
    promotion.begin(party, strings);
    REQUIRE(promotion.active());
    TowerPromotion::LocalSnapshot history;
    promotion.saveLocal(history);
    const auto initial = native(promotion, resources);
    REQUIRE(promotion.shown() == 0);
    promotion.update(1, false);
    REQUIRE(promotion.shown() == 0);
    CHECK(native(promotion, resources) != initial);
    CHECK(captured(promotion, history, resources) == initial);
    REQUIRE(promotion.restoreLocal(history));
    CHECK(native(promotion, resources) == initial);
    promotion.clear();
    NativeStateDigest stale(resources.resources());
    CHECK_FALSE(promotion.appendLocalDigest(stale, history));
    CHECK_FALSE(stale.checkedFinish());
}

TEST_CASE("speculative first victory artwork cannot contaminate an earlier native snapshot",
          "[native-state-digest][scene-travel-native-digest][local-rewind]") {
    Travel fixture("travel-digest-victory");
    BossVictoryPresentation victory;
    BossVictoryPresentation::LocalSnapshot before;
    victory.saveLocal(before);
    const auto empty = native(victory, fixture.resources);
    victory.begin(41, 'G', 1, 0, false, Vec3{0});
    victory.bindWizard(fixture.device, fixture.items);
    fixture.refresh();
    CHECK(native(victory, fixture.resources) != empty);
    CHECK(captured(victory, before, fixture.resources) == empty);
    REQUIRE(victory.restoreLocal(before));
    CHECK(native(victory, fixture.resources) == empty);
    victory.begin(41, 'G', 1, 0, false, Vec3{0});
    victory.bindWizard(fixture.device, fixture.items);
    const MessageTable strings;
    victory.update(1, kStep, false, strings);
    const auto stable = native(victory, fixture.resources);
    for (const f32 alpha : {-1.0f, 0.0f, 0.37f, 1.0f}) {
        victory.drawWizard(fixture.device, Mat4{1}, {}, nullptr, alpha);
        CHECK(native(victory, fixture.resources) == stable);
    }
    BossVictory state;
    state.begin(41, 'G', 1, 0, false);
    const auto initial = native(state, fixture.resources);
    const auto stage = state.stage();
    state.update(1, {});
    REQUIRE(state.stage() == stage);
    CHECK(native(state, fixture.resources) != initial);
}

TEST_CASE("empty scene phase owners still hash latent native clocks and reject foreign history",
          "[native-state-digest][scene-travel-native-digest]") {
    const NativeStateResources resources;
    BossSequence boss;
    const BossSequence other;
    BossSequence::LocalSnapshot saved;
    boss.saveLocal(saved);
    const auto initial = native(boss, resources);
    CHECK(captured(boss, saved, resources) == initial);
    NativeStateDigest foreign(resources.resources());
    CHECK_FALSE(other.appendLocalDigest(foreign, saved));
    CHECK_FALSE(foreign.checkedFinish());
    boss.clear();
    NativeStateDigest stale(resources.resources());
    CHECK_FALSE(boss.appendLocalDigest(stale, saved));
    CHECK_FALSE(stale.checkedFinish());

    const TowerRelics relics;
    TowerRelics::LocalSnapshot relicHistory;
    relics.saveLocal(relicHistory);
    CHECK(native(relics, resources) == captured(relics, relicHistory, resources));
    const SumnerFigure wizard;
    SumnerFigure::LocalSnapshot wizardHistory;
    wizard.saveLocal(wizardHistory);
    CHECK(native(wizard, resources) == captured(wizard, wizardHistory, resources));
    PortalDeparture departure;
    PortalDeparture::LocalSnapshot departureHistory;
    departure.saveLocal(departureHistory);
    test::FakeRenderDevice device;
    TextureSet textures;
    departure.begin(device, textures);
    departure.update(1);
    CHECK(native(departure, resources) != captured(departure, departureHistory, resources));
    REQUIRE(departure.restoreLocal(departureHistory));
    CHECK(native(departure, resources) == captured(departure, departureHistory, resources));
}

TEST_CASE("hint menu and cursor digests retain hidden progression and typed native layout",
          "[native-state-digest][scene-travel-native-digest][local-rewind]") {
    const NativeStateResources resources;
    SumnerHints hints;
    const auto hintFile = test::scratchDirectory("native-hint-cursor") / "hints.json";
    writeTextFile(hintFile,
                  R"({"messages":[{"name":"G1","lines":["Guardian"]}],
                       "lists":[{"name":"BOSS_HINTS","messages":[0]}]})");
    REQUIRE(hints.load(hintFile));
    HintKnowledge knowledge;
    knowledge.worldsOpen = 1U << 7U;
    hints.next(HintTopic::Guardians, knowledge, "GENERAL");
    SumnerHints::LocalSnapshot hintHistory;
    hints.saveLocal(hintHistory);
    const auto initialHint = native(hints, resources);
    const auto visibleIndex = hints.guardian();
    hints.next(HintTopic::Guardians, knowledge, "GENERAL");
    REQUIRE(hints.guardian() == visibleIndex);
    CHECK(native(hints, resources) != initialHint); // the hidden search has widened
    CHECK(captured(hints, hintHistory, resources) == initialHint);
    REQUIRE(hints.restoreLocal(hintHistory));
    CHECK(native(hints, resources) == initialHint);

    const auto makeFont = [] {
        std::vector<BitmapGlyph> glyphs;
        for (s32 code = 33; code < 127; ++code) {
            glyphs.push_back({code, 8, 0, 0});
        }
        return BitmapFont::fromGlyphs(10, 4, std::move(glyphs));
    };
    auto firstFont = makeFont();
    auto secondFont = makeFont();
    test::FakeTexture firstSheet{64, 64};
    test::FakeTexture secondSheet{64, 64};
    TextPainter firstPainter;
    TextPainter secondPainter;
    firstPainter.setFont(&firstFont, &firstSheet);
    secondPainter.setFont(&secondFont, &secondSheet);
    HintMenu first;
    HintMenu second;
    const HintMenuLabels labels{"HELP", {{"GENERAL", 39}, {"GUARDIANS", 40}}, "SELECT", "PLAYER"};
    REQUIRE(first.open(firstPainter, {}, labels));
    REQUIRE(second.open(secondPainter, {}, labels));
    REQUIRE(native(first, resources) == native(second, resources));
    HintMenu::LocalSnapshot saved;
    first.saveLocal(saved);
    const auto baseline = native(first, resources);
    test::FakeRenderDevice device;
    first.update(device, {}, 1);
    REQUIRE(first.topics().selection() == second.topics().selection());
    CHECK(native(first, resources) != baseline);
    CHECK(captured(first, saved, resources) == baseline);
    first.read(firstPainter, "ANSWER", {"FIRST", "SECOND"}, 0.8f, false, 16);
    CHECK(native(first, resources) != baseline);
    REQUIRE(first.restoreLocal(saved));
    CHECK(native(first, resources) == baseline);
    Canvas canvas;
    canvas.begin(device, Mat4{1});
    first.draw(canvas, firstPainter);
    canvas.end();
    CHECK(native(first, resources) == baseline);
    NativeStateDigest foreign(resources.resources());
    CHECK_FALSE(second.appendLocalDigest(foreign, saved));
    CHECK_FALSE(foreign.checkedFinish());

    const SumnerVisit visit;
    SumnerVisit::LocalSnapshot visitHistory;
    visit.saveLocal(visitHistory);
    CHECK(native(visit, resources) == captured(visit, visitHistory, resources));
}

TEST_CASE("travel native digest bytes stay pinned for bound portals pads arrival and departure",
          "[native-state-digest][scene-travel-native-digest][native-digest-pin]") {
    Travel fixture("travel-digest-pin");
    fixture.exits.setAlpha("g1", 0.75f);
    fixture.departure.begin(fixture.device, fixture.items.textures);
    const auto digests = fixture.digest();
    // SHA-256 of the native digest of each owner above, which netplay peers must hash alike: a
    // literal moves only when what is visited changes on purpose.
    constexpr std::string_view kExits =
        "daea92513cff152dcefbe2a299124e765392941a8365312c2242fe3a2e2a07a2";
    constexpr std::string_view kPads =
        "cb2dcd7002e0d3e75f1a440c768c7e1cf3b6df6e6a2bf524afe62aa1db2c863b";
    constexpr std::string_view kArrival =
        "a078f4d3f965356cd757f8df5c4d2137c9b14f4b178a7cb706e57f1ec75ed561";
    constexpr std::string_view kDeparture =
        "47d72be36f2dde3920a0c401fe2b6fd209ac8aba1585c567fefec196c86fad87";
    CHECK(test::hexBytes(digests[0]) == kExits);
    CHECK(test::hexBytes(digests[1]) == kPads);
    CHECK(test::hexBytes(digests[2]) == kArrival);
    CHECK(test::hexBytes(digests[3]) == kDeparture);
}

TEST_CASE("travel native digest bytes stay pinned after the travel script and what it saved",
          "[native-state-digest][scene-travel-native-digest][digest-golden]") {
    Travel fixture("travel-digest-golden");
    // The departure begins at tick 100, so it is under way on both sides of the save.
    for (s32 tick = 0; tick < 102; ++tick) {
        fixture.step(tick, false);
    }
    Travel::Saved saved;
    fixture.save(saved);
    for (s32 tick = 102; tick < 110; ++tick) {
        fixture.step(tick, false);
    }
    // SHA-256 of each owner's native digest 110 ticks in and of what it saved 102 ticks in,
    // which netplay peers must hash alike: a literal moves only when what is visited changes on
    // purpose.
    test::checkPinnedDigests("ExitPortals", native(fixture.exits, fixture.resources),
                             captured(fixture.exits, saved.exits, fixture.resources),
                             "efffba21c402bc5d1853d967f8ddacc43030f69325910f6335564697c350778e",
                             "acb34213065a6dda325b06118ebb31f8773bc196b5d6dfa29f50c1108456e3cf");
    test::checkPinnedDigests("LevelTransporters", native(fixture.pads, fixture.resources),
                             captured(fixture.pads, saved.pads, fixture.resources),
                             "47d69b1c5fa30a9cf103e355f2c6731960655d48dfc676f893c9ad1b71e6aca3",
                             "ce526898296771353ed200ce6e4f67b4192c6359cfc734b4caa0383ec5c97347");
    test::checkPinnedDigests("PortalDeparture", native(fixture.departure, fixture.resources),
                             captured(fixture.departure, saved.departure, fixture.resources),
                             "decfa43dc3622535eaf7d3d17671bc0638374bef5aa4d4a8734fe9c13612abf8",
                             "e5ccce677d278d44567752da1e3419c5acea7906be528d47a8ce726c29c091a1");
}

TEST_CASE("arrival and victory digest bytes stay pinned after the travel script and their saves",
          "[native-state-digest][scene-travel-native-digest][digest-golden]") {
    Travel fixture("travel-presentation-golden");
    BossVictoryPresentation victory;
    victory.begin(41, 'G', 1, 0, false, Vec3{0});
    victory.bindWizard(fixture.device, fixture.items);
    fixture.refresh();
    const MessageTable strings;
    const auto step = [&](s32 tick) {
        fixture.step(tick, false);
        victory.capturePresentation();
        victory.update(1, kStep, false, strings);
    };
    for (s32 tick = 0; tick < 4; ++tick) {
        step(tick);
    }
    Travel::Saved saved;
    fixture.save(saved);
    BossVictoryPresentation::LocalSnapshot victorySaved;
    victory.saveLocal(victorySaved);
    for (s32 tick = 4; tick < 12; ++tick) {
        step(tick);
    }
    // SHA-256 of each owner's native digest twelve ticks in and of what it saved four ticks in,
    // which netplay peers must hash alike: a literal moves only when what is visited changes on
    // purpose.
    test::checkPinnedDigests("LevelArrivalPresentation", native(fixture.arrival, fixture.resources),
                             captured(fixture.arrival, saved.arrival, fixture.resources),
                             "6adaa8a9cfd183965b60ff5dcca883719d342ff133fc2f5a787da710774e3f06",
                             "48ce4f0c371589edacdff1ab7bcd383fc09341ab760b82b8ea1b8630c23a88e0");
    test::checkPinnedDigests("BossVictoryPresentation", native(victory, fixture.resources),
                             captured(victory, victorySaved, fixture.resources),
                             "0c0f521a76b942a3761c4641901f12f4b0da9690c9a829fda67037072c7d1a45",
                             "434ca2a03f07246aca45495f740ffe3e7530121d49322109888c951651bcb34e");
}

TEST_CASE("promotion and hint digest bytes stay pinned after their scripts and what they saved",
          "[native-state-digest][scene-travel-native-digest][digest-golden]") {
    const NativeStateResources resources;
    std::array<PlayerRuntime, 1> party;
    CharacterSave save;
    save.progress().experience = levelExperience(30);
    save.progress().promotedLevel = 29;
    party[0].actor.spawn(0, save, nullptr, Vec3{0}, 0);
    const MessageTable strings;
    TowerPromotion promotion;
    promotion.begin(party, strings);
    REQUIRE(promotion.active());
    for (s32 tick = 0; tick < 3; ++tick) {
        promotion.update(1, false);
    }
    TowerPromotion::LocalSnapshot promotionSaved;
    promotion.saveLocal(promotionSaved);
    for (s32 tick = 3; tick < 8; ++tick) {
        promotion.update(1, false);
    }

    SumnerHints hints;
    const auto hintFile = test::scratchDirectory("native-hint-golden") / "hints.json";
    writeTextFile(hintFile,
                  R"({"messages":[{"name":"G1","lines":["Guardian"]}],
                       "lists":[{"name":"BOSS_HINTS","messages":[0]}]})");
    REQUIRE(hints.load(hintFile));
    HintKnowledge knowledge;
    knowledge.worldsOpen = 1U << 7U;
    hints.next(HintTopic::Guardians, knowledge, "GENERAL");
    SumnerHints::LocalSnapshot hintsSaved;
    hints.saveLocal(hintsSaved);
    hints.next(HintTopic::Guardians, knowledge, "GENERAL"); // the hidden search widens

    std::vector<BitmapGlyph> glyphs;
    for (s32 code = 33; code < 127; ++code) {
        glyphs.push_back({code, 8, 0, 0});
    }
    const auto font = BitmapFont::fromGlyphs(10, 4, std::move(glyphs));
    const test::FakeTexture sheet{64, 64};
    TextPainter painter;
    painter.setFont(&font, &sheet);
    HintMenu menu;
    const HintMenuLabels labels{"HELP", {{"GENERAL", 39}, {"GUARDIANS", 40}}, "SELECT", "PLAYER"};
    REQUIRE(menu.open(painter, {}, labels));
    test::FakeRenderDevice device;
    menu.update(device, {}, 1);
    HintMenu::LocalSnapshot menuSaved;
    menu.saveLocal(menuSaved);
    for (s32 tick = 1; tick < 4; ++tick) {
        menu.update(device, {}, 1);
    }
    // SHA-256 of each owner's native digest where its script ends and of what it saved partway
    // (the promotion three of eight ticks in, the hints before their second search, the menu one
    // of four ticks in), which netplay peers must hash alike: a literal moves only when what is
    // visited changes on purpose.
    test::checkPinnedDigests("TowerPromotion", native(promotion, resources),
                             captured(promotion, promotionSaved, resources),
                             "46149bf785093593ed13da4830af0db7d065a7801a0420b8df9639f2ead6b357",
                             "7283c274a50682e3716cd86245b85b3003520cd4c46e147bc490142fb99b6d1d");
    test::checkPinnedDigests("SumnerHints", native(hints, resources),
                             captured(hints, hintsSaved, resources),
                             "7ee46d12f50a3f3ffe8466d32448c5ea6ed4ff436d2e2f343884dec9f1ab712c",
                             "d75dc3408a49f0940d20926001a09b1961fd5d91f4461a85f0debc87dd203ea2");
    test::checkPinnedDigests("HintMenu", native(menu, resources),
                             captured(menu, menuSaved, resources),
                             "b4be11972c05566e62e7bff42916fff844da73942e6e0f5f2287628b411e8c85",
                             "f7a86eabd54019e312d542a5f9deed31383a1894ae30de49f9a9587935fc765f");
}
} // namespace
