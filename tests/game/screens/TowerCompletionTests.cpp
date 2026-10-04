#include <array>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/SoundSet.h"
#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/app/Scenario.h"
#include "game/players/CharacterSave.h"
#include "game/screens/TowerCompletion.h"
#include "game/screens/TowerRelics.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Kind = TowerCompletion::Kind;

TEST_CASE("completion scenarios stage the intended route and Dream World arrival",
          "[game][tower-completion][scenario]") {
    const auto directory = test::dataDirectory().parent_path() / "tests/scenarios";
    for (const auto& [file, expected] :
         std::array{std::pair{"tower-completion-window.json", Kind::Window},
                    std::pair{"tower-completion-runes.json", Kind::Underworld},
                    std::pair{"tower-completion-garm.json", Kind::Garm}}) {
        INFO(file);
        const auto scenario = Scenario::load(directory / file);
        const auto party = scenario.partyMembers();
        REQUIRE(party.size() == 1);
        const std::array collection{party[0].save.progress().relics};
        REQUIRE(TowerCompletion::pending(collection) == TowerCompletion::bit(expected));
        if (expected == Kind::Window) {
            REQUIRE(scenario.tower.arrivalWorld == 10);
            REQUIRE(party[0].save.progress().crystals[8] >= LevelTriggers::crystalsNeeded(8));
        }
    }
}

TEST_CASE("tower completion follows actual new party acquisitions, not ownership alone",
          "[game][tower-completion]") {
    std::array<Relics, 2> party;
    REQUIRE(TowerCompletion::pending(party) == 0);
    party[0].runes = 0x1fff;
    party[0].shards = 0x7fe;
    REQUIRE(TowerCompletion::pending(party) == 0);
    party[1].addRune(12);
    party[1].addShard(8);
    REQUIRE(TowerCompletion::pending(party) == 0);
    party[0] = {};
    REQUIRE(TowerCompletion::pending(party) == TowerCompletion::bit(Kind::MoreShards));
    party[0].shards = 0xfe;
    REQUIRE(TowerCompletion::pending(party) == TowerCompletion::bit(Kind::Window));
    party[0].runes = 0xfff;
    REQUIRE(TowerCompletion::pending(party) ==
            (TowerCompletion::bit(Kind::Window) | TowerCompletion::bit(Kind::Garm)));
}

TEST_CASE("the twelfth rune and temple victory can arrive in either order",
          "[game][tower-completion]") {
    std::array<Relics, 1> party;
    auto& relics = party[0];
    relics.runes = 0x7ff;
    relics.shards = 0x1fe;
    REQUIRE(relics.addRune(11));
    REQUIRE(TowerCompletion::pending(party) == TowerCompletion::bit(Kind::TwelveWaiting));
    SECTION("Skorne is defeated after the twelve stones were installed") {
        relics.pendingRunes = 0;
        REQUIRE(relics.addShard(9));
        REQUIRE(relics.pendingShards == 0x200);
    }
    SECTION("Skorne was defeated before the twelfth stone") {
        relics.shards |= 0x200;
    }
    REQUIRE(TowerCompletion::pending(party) == TowerCompletion::bit(Kind::Underworld));
    relics.pendingRunes = relics.pendingShards = 0;
    REQUIRE(TowerCompletion::pending(party) == 0);
}

TEST_CASE("a thirteenth stone alone does not open the completed rune route",
          "[game][tower-completion]") {
    std::array<Relics, 1> party;
    party[0].addRune(12);
    REQUIRE(TowerCompletion::pending(party) == 0);
    party[0].runes |= 0xfff;
    REQUIRE(TowerCompletion::pending(party) == TowerCompletion::bit(Kind::Garm));
}

TEST_CASE("interrupted completion acknowledgements survive saves without granting relics",
          "[game][tower-completion][save]") {
    CharacterSave save;
    save.name = "TEST";
    auto& relics = save.progress().relics;
    relics.runes = 0xfff;
    relics.shards = 0x1fe;
    relics.pendingCeremonies =
        TowerCompletion::bit(Kind::Window) | TowerCompletion::bit(Kind::TwelveWaiting);
    const auto loaded = CharacterSave::fromJson(save.toJson());
    REQUIRE(loaded.progress().relics == relics);
    const std::array party{loaded.progress().relics};
    REQUIRE(TowerCompletion::pending(party) == relics.pendingCeremonies);
    TowerRelics::acknowledge(relics, {TowerRelics::Kind::Followup, static_cast<s32>(Kind::Window)});
    REQUIRE(relics.pendingCeremonies == TowerCompletion::bit(Kind::TwelveWaiting));
    REQUIRE(relics.runes == 0xfff);
    REQUIRE(relics.shards == 0x1fe);
    const auto legacy = CharacterSave::fromJson(R"({"name":"OLD","character":0,
        "classes":{"WAR":{"relics":{"runes":8191,"shards":2046}}}})");
    REQUIRE(legacy.progress().relics.pendingCeremonies == 0);
    const auto invalid = CharacterSave::fromJson(R"({"name":"BAD","character":0,
        "classes":{"WAR":{"relics":{"pendingCeremonies":65535}}}})");
    REQUIRE(invalid.progress().relics.pendingCeremonies == TowerCompletion::kKnown);
    const std::array invalidParty{invalid.progress().relics};
    REQUIRE((TowerCompletion::pending(invalidParty) &
             (TowerCompletion::bit(Kind::Window) | TowerCompletion::bit(Kind::Underworld) |
              TowerCompletion::bit(Kind::Garm))) == 0);
}

TEST_CASE("a pending waiting speech upgrades when the party has now defeated Skorne",
          "[game][tower-completion]") {
    std::array<Relics, 1> party;
    party[0].runes = 0xfff;
    party[0].shards = 0x3fe;
    party[0].pendingCeremonies = TowerCompletion::bit(Kind::TwelveWaiting);
    REQUIRE(TowerCompletion::pending(party) == TowerCompletion::bit(Kind::Underworld));
}

TEST_CASE("tower completion reveals then speaks every page once and waits for the voice",
          "[game][tower-completion]") {
    const auto file = test::scratchDirectory("tower-completion") / "text.json";
    writeTextFile(file, R"({"messages":[{"name":"ALLSHARDS","lines":["First","Last"]}]})");
    MessageTable strings;
    REQUIRE(strings.load(file));
    std::array<Relics, 1> party;
    party[0].shards = 0x1fe;
    party[0].pendingCeremonies = TowerCompletion::bit(Kind::Window);
    TowerRelics ceremony;
    ceremony.begin(party, strings);
    REQUIRE(ceremony.phase() == TowerRelics::Phase::Reveal);
    REQUIRE(ceremony.revealAlpha(Kind::Window) == 0);
    REQUIRE(ceremony.update(-60, -1, false).voice.empty());
    REQUIRE(ceremony.revealAlpha(Kind::Window) == 0);
    REQUIRE(ceremony.update(90, 1.5f, false).voice.empty());
    REQUIRE(ceremony.revealAlpha(Kind::Window) == Catch::Approx(0.5f));
    REQUIRE(ceremony.update(90, 1.5f, false).voice.empty());
    REQUIRE(ceremony.phase() == TowerRelics::Phase::Speech);
    REQUIRE(ceremony.revealAlpha(Kind::Window) == 1);
    REQUIRE(ceremony.update(119, 0, false).voice.empty());
    REQUIRE(ceremony.update(1, 0, false).voice == "S_4KEYVOX");
    REQUIRE(ceremony.caption() == "First");
    REQUIRE(ceremony.update(69, 0, true).voice.empty());
    REQUIRE(ceremony.captionPage() == 0);
    REQUIRE_FALSE(ceremony.update(1, 0, true).completed);
    REQUIRE(ceremony.caption() == "Last");
    REQUIRE_FALSE(ceremony.update(500, 0, true).completed);
    REQUIRE(ceremony.active());
    const auto completed = ceremony.update(0, 0, false).completed;
    REQUIRE(completed.has_value());
    REQUIRE(completed->followup() == Kind::Window);
    REQUIRE(ceremony.pendingCeremonies() == 0);
    REQUIRE_FALSE(ceremony.active());
    REQUIRE_FALSE(ceremony.update(1000, 0, false).completed);
    REQUIRE(ceremony.figures().count() == 0);
}

TEST_CASE("completion reveal timing is tick-based at different update rates",
          "[game][tower-completion]") {
    for (const s32 ticks : {1, 2, 4, 6, 30}) {
        INFO(ticks);
        const MessageTable strings;
        std::array<Relics, 1> party;
        party[0].runes = 0x1fff;
        party[0].pendingCeremonies = TowerCompletion::bit(Kind::Garm);
        TowerRelics ceremony;
        ceremony.begin(party, strings);
        for (s32 elapsed = 0; elapsed < TowerCompletion::kRevealTicks; elapsed += ticks) {
            REQUIRE(ceremony.phase() == TowerRelics::Phase::Reveal);
            ceremony.update(ticks, static_cast<f32>(ticks) / 60, false);
        }
        REQUIRE(ceremony.phase() == TowerRelics::Phase::Speech);
        REQUIRE(ceremony.revealAlpha(Kind::Garm) == 1);
        REQUIRE(ceremony.update(120, 2, false).voice == "S_RUNE13YES");
    }
}

TEST_CASE("completion captions voices and reveal cameras exist in the shipped tower",
          "[game][tower-completion][assets]") {
    const auto root = test::assetOrSkip("TEXT/scroll_e.rom").parent_path().parent_path();
    MessageTable strings;
    REQUIRE(strings.load(root / "TEXT/scroll_e.rom"));
    WorldLayout layout;
    REQUIRE(layout.load(root / "LEVELS/LEVELL1"));
    SoundSet wizard;
    SoundSet tower;
    REQUIRE(wizard.load(root / "audio/WIZTOWER"));
    REQUIRE(tower.load(root / "audio/TOWAMB"));
    for (const auto kind :
         {Kind::MoreShards, Kind::Window, Kind::TwelveWaiting, Kind::Underworld, Kind::Garm}) {
        INFO(TowerCompletion::message(kind));
        const auto message = strings.find(TowerCompletion::message(kind));
        REQUIRE(message.has_value());
        REQUIRE_FALSE(strings.message(*message).pages.empty());
        const auto voice = TowerCompletion::voice(kind);
        REQUIRE((wizard.find(voice).has_value() || tower.find(voice).has_value()));
        if (TowerCompletion::reveals(kind)) {
            REQUIRE(layout.findLocator(LocatorKind::TriggerCamera, TowerCompletion::camera(kind)) !=
                    nullptr);
        }
    }
}

TEST_CASE("tower completion runs after arrival and promotions and releases control only at the end",
          "[game][tower-completion][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root));
    LevelCatalog levels;
    REQUIRE(levels.load(root));
    const auto scenario = Scenario::fromJson(R"({"welcome":false,"arrivalWorld":10,
        "party":[{"class":"WAR","level":20,
        "crystals":[0,15,100,125,150,175,200,225,250],
        "runes":[0,1,2,3,4,5,6,7,8,9,10],"newRunes":[11,12],
        "shards":[1,2,3,4,5,6,7,9,10],"newShards":[8]}]})");
    auto party = scenario.partyMembers();
    party[0].save.progress().promotedLevel = 19;
    const GameConfig config;
    GameContext context;
    context.config = &config;
    context.levels = &levels;
    context.unpackedRoot = root;
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, party, scenario.tower));
    const auto route = [&](std::string_view tag) -> const ExitPortals::Portal& {
        for (usize i = 0; i < scene.portals().size(); ++i) {
            if (scene.portals().portal(i).tag == tag) {
                return scene.portals().portal(i);
            }
        }
        FAIL("missing tower route");
        return scene.portals().portal(0);
    };
    REQUIRE(route("e1").alpha == 0);
    REQUIRE(route("f1").alpha == 0);
    REQUIRE(route("h4").alpha == 0);
    const f32 initialGarmHeight = route("h4").position.y;
    REQUIRE(route("h4").support >= 0);
    const Vec3 position = scene.actor(0)->position();
    REQUIRE(world.startPoint(8) != nullptr);
    REQUIRE(world.startPoint(0) != nullptr);
    REQUIRE(glm::distance(position, world.startPoint(8)->position) < 5);
    REQUIRE(glm::distance(position, world.startPoint(0)->position) > 20);
    PlayScene::Inputs inputs{};
    inputs[0].move = MoveInput{Vec2{1, 0}, 1};
    std::vector<Kind> reveals;
    std::vector<Kind> speeches;
    s32 frames = 0;
    bool promotionSeen = false;
    while (scene.towerRelics().active() && frames++ < 5000) {
        REQUIRE_FALSE(scene.canPause(0));
        REQUIRE_FALSE(scene.canJoin(1));
        scene.update(1.0 / 30, inputs);
        REQUIRE(scene.actor(0)->position() == position);
        const auto& ceremony = scene.towerRelics();
        const auto* entry = ceremony.current();
        if (scene.actor(0)->save().progress().promotedLevel == 20) {
            promotionSeen = true;
        }
        if (entry == nullptr || entry->kind != TowerRelics::Kind::Followup) {
            continue;
        }
        REQUIRE(promotionSeen);
        auto& seen = ceremony.phase() == TowerRelics::Phase::Reveal ? reveals : speeches;
        if (seen.empty() || seen.back() != entry->followup()) {
            seen.push_back(entry->followup());
        }
        if (ceremony.phase() == TowerRelics::Phase::Reveal) {
            const auto* camera = world.layout().findLocator(
                LocatorKind::TriggerCamera, TowerCompletion::camera(entry->followup()));
            REQUIRE(camera != nullptr);
            REQUIRE(scene.viewCamera().position == camera->position);
        } else {
            // Dream World's event marker is 8. The shard and rune speeches
            // use its authored cameras, not Sumner's central lookout.
            const u32 cameraId = entry->followup() == Kind::Window ? 228U : 178U;
            const auto* camera = world.layout().findLocator(LocatorKind::TriggerCamera, cameraId);
            REQUIRE(camera != nullptr);
            REQUIRE(scene.viewCamera().position == camera->position);
        }
    }
    REQUIRE(frames < 5000);
    REQUIRE(reveals == std::vector<Kind>{Kind::Window, Kind::Underworld, Kind::Garm});
    REQUIRE(speeches == reveals);
    REQUIRE(scene.canPause(0));
    REQUIRE(route("e1").alpha == 1);
    REQUIRE(route("f1").alpha == 1);
    REQUIRE(route("h4").alpha == 1);
    REQUIRE_FALSE(route("e1").shut);
    REQUIRE_FALSE(route("f1").shut);
    REQUIRE_FALSE(route("h4").shut);
    CHECK(route("h4").position.y > initialGarmHeight + 5);
    const auto garmFloor = world.collision().floorAt(route("h4").position, 0.5f, 1);
    REQUIRE(garmFloor);
    CHECK(route("h4").position.y == Catch::Approx(garmFloor->y + ExitPortals::kFloorLift));
    bool lift = false;
    for (usize i = 0; i < world.triggers().size(); ++i) {
        const auto& trigger = world.triggers().trigger(i);
        if (trigger.id == 255) {
            REQUIRE(trigger.forced);
            REQUIRE(world.triggers().settled(trigger.target));
            lift = true;
        }
    }
    REQUIRE(lift);
    const auto saved = scene.party();
    const auto& relics = saved[0].save.progress().relics;
    REQUIRE(relics.runes == 0x1fff);
    REQUIRE(relics.shards == 0x7fe);
    REQUIRE(relics.pendingRunes == 0);
    REQUIRE(relics.pendingShards == 0);
    REQUIRE(relics.pendingCeremonies == 0);
    REQUIRE(scene.towerRelics().figures().count() == 21);
    scene.close();
    REQUIRE(scene.open(device, context, world, saved, scenario.tower));
    REQUIRE_FALSE(scene.towerRelics().active());
    REQUIRE(route("e1").alpha == 1);
    REQUIRE(scene.towerRelics().figures().count() == 21);
    scene.close();
}

TEST_CASE("saving between final shard placement and speech resumes only the follow-up",
          "[game][tower-completion][assets]") {
    const auto root =
        test::assetOrSkip("LEVELS/LEVELL1/WORLDS.PS2").parent_path().parent_path().parent_path();
    test::FakeRenderDevice device;
    LevelWorld world;
    REQUIRE(world.load(device, root));
    const auto scenario = Scenario::fromJson(R"({"welcome":false,"arrivalWorld":7,
        "party":[{"class":"WAR","level":20,"shards":[1,2,3,4,5,6,7],"newShards":[8]}]})");
    const GameConfig config;
    GameContext context;
    context.config = &config;
    context.unpackedRoot = root;
    PlayScene scene;
    REQUIRE(scene.open(device, context, world, scenario.partyMembers(), scenario.tower));
    s32 frames = 0;
    while (scene.towerRelics().current()->kind != TowerRelics::Kind::Followup && frames++ < 2000) {
        scene.update(1.0 / 30, {});
    }
    REQUIRE(frames < 2000);
    auto saved = scene.party();
    saved[0].save = CharacterSave::fromJson(saved[0].save.toJson());
    REQUIRE(saved[0].save.progress().relics.pendingShards == 0);
    REQUIRE(saved[0].save.progress().relics.pendingCeremonies ==
            TowerCompletion::bit(Kind::Window));
    scene.close();
    REQUIRE(scene.open(device, context, world, saved, scenario.tower));
    REQUIRE(scene.towerRelics().current()->kind == TowerRelics::Kind::Followup);
    REQUIRE(scene.towerRelics().figures().count() == 8);
    frames = 0;
    while (scene.towerRelics().active() && frames++ < 2000) {
        scene.update(1.0 / 30, {});
    }
    REQUIRE(frames < 2000);
    REQUIRE(scene.party()[0].save.progress().relics.pendingCeremonies == 0);
    REQUIRE(scene.towerRelics().figures().count() == 8);
    scene.close();
}
} // namespace
