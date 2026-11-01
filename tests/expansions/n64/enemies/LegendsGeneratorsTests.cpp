#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <tuple>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "expansions/n64/enemies/LegendsEnemyAssets.h"
#include "expansions/n64/enemies/LegendsGenerators.h"
#include "expansions/n64/level/LegendsUnlocks.h"
#include "expansions/shared/scenes/LegendsLevel.h"
#include "expansions/shared/scenes/LegendsSceneAdapter.h"
#include "expansions/shared/scenes/LegendsTraversal.h"
#include "game/effects/EnemyFeedbackEmission.h"
#include "game/enemies/LevelOpponents.h"
#include "game/party/PlayerAttacks.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

void be16(std::vector<u8>& bytes, usize at, u16 value) {
    bytes.at(at) = static_cast<u8>(value >> 8U);
    bytes.at(at + 1) = static_cast<u8>(value);
}
void be32(std::vector<u8>& bytes, usize at, u32 value) {
    be16(bytes, at, static_cast<u16>(value >> 16U));
    be16(bytes, at + 2, static_cast<u16>(value));
}
void real(std::vector<u8>& bytes, usize at, f32 value) {
    be32(bytes, at, std::bit_cast<u32>(value));
}

std::vector<u8> nativeTemplates() {
    std::vector<u8> bytes(4 + 8 * 56);
    be32(bytes, 0, 8);
    for (usize i = 0; i < 8; ++i) {
        const usize at = 4 + i * 56;
        const u8 tier = static_cast<u8>(i % 4);
        be16(bytes, at, i < 4 ? 0x0605 : 0x0613);
        bytes[at + 8] = 18;
        bytes[at + 9] = 2;
        bytes[at + 10] = tier;
        bytes[at + 11] = tier == 0 ? 0xff : static_cast<u8>(tier - 1);
        const std::string model = std::string(i < 4 ? "GGR" : "GIG") + char('0' + tier);
        std::ranges::copy(model, bytes.begin() + static_cast<std::ptrdiff_t>(at + 12));
        be32(bytes, at + 28, 1);
        real(bytes, at + 32, 2.5f);
        real(bytes, at + 36, 6);
    }
    return bytes;
}

std::vector<u8> mixedTemplates() {
    std::vector<u8> bytes(4 + 3 * 56);
    be32(bytes, 0, 3);
    for (usize tier = 0; tier < 3; ++tier) {
        const usize at = 4 + tier * 56;
        be16(bytes, at, 0x0663);
        bytes[at + 8] = 28;
        bytes[at + 9] = 10;
        bytes[at + 10] = static_cast<u8>(tier);
        bytes[at + 11] = tier == 0 ? 0xff : tier == 1 ? 2 : 0;
        const auto model = std::string("GSP") + char('0' + tier);
        std::ranges::copy(model, bytes.begin() + static_cast<std::ptrdiff_t>(at + 12));
        be32(bytes, at + 28, tier == 0 ? 5 : 3);
        real(bytes, at + 32, 3);
        real(bytes, at + 36, 3);
        real(bytes, at + 40, 3);
    }
    return bytes;
}

formats::LegendsN64Fixture nativeFixture() {
    formats::LegendsN64Fixture fixture;
    fixture.kind = formats::LegendsN64FixtureKind::Generator;
    fixture.nativeIndex = 71;
    fixture.templateKey = 0x0605;
    fixture.generatorTier = 3;
    fixture.minimumPlayers = 2;
    fixture.position = {4, 2, 7};
    fixture.facingRadians = .75f;
    return fixture;
}

// Independent source audit: use the executable's level table and authored PGEn
// names, not the adapter's roster multiplier or its runtime health values.
std::array<s32, 3> nativeHealthOf(const LegendsGenerator& generator, std::span<const u8> templates,
                                  std::span<const u8> program, const LegendsStageUnlock& stage) {
    s32 numerator = 0;
    for (usize at = 0xe0045ca8U - 0xe0000400U; program[at] != 0xff; at += 5) {
        if (program[at] == stage.realm && program[at + 1] == stage.stage) {
            numerator = program[at + 3] + 7;
            break;
        }
    }
    REQUIRE(numerator >= 8);
    std::array<s32, 3> health{};
    for (s32 state = 1; state <= generator.gameplay.tier; ++state) {
        const auto& sequence = generator.sequences[static_cast<usize>(state)];
        REQUIRE(sequence.size() == 14);
        bool found = false;
        for (usize i = 0; i < readU32BE(templates, 0); ++i) {
            const usize at = 4 + i * 56;
            if (std::equal(sequence.begin() + 10, sequence.end(),
                           templates.begin() + static_cast<std::ptrdiff_t>(at + 12))) {
                health[static_cast<usize>(state - 1)] = templates[at + 8] * numerator / 8;
                found = true;
                break;
            }
        }
        REQUIRE(found);
    }
    return health;
}

TEST_CASE("N64 generators resolve their native level species and damage artwork",
          "[legends][legends-generators]") {
    auto bytes = nativeTemplates();
    auto fixture = nativeFixture();
    std::string warning;
    const auto ice = legendsN64Generator(fixture, bytes, "ICE1", {}, &warning);
    REQUIRE(ice);
    CHECK(warning.empty());
    CHECK(ice->gameplay.nativeIndex == 71);
    CHECK(ice->gameplay.kind == 16);
    CHECK(ice->gameplay.tier == 3);
    CHECK(ice->gameplay.minimumPlayers == 2);
    CHECK(ice->gameplay.position == fixture.position);
    CHECK(ice->gameplay.yaw == fixture.facingRadians);
    CHECK(ice->gameplay.body.radius == 2.5f);
    CHECK(ice->gameplay.body.height == 6);
    CHECK(ice->gameplay.body.hitPoints == 12);
    CHECK(ice->gameplay.body.armor == 0);
    CHECK(ice->gameplay.stageHealth == std::array<s32, 3>{12, 12, 12});
    CHECK(ice->sequences == std::array<std::string, 4>{"GENERATOR:GIG0", "GENERATOR:GIG1",
                                                       "GENERATOR:GIG2", "GENERATOR:GIG3"});
    const auto castle = legendsN64Generator(fixture, bytes, "castle1", {});
    REQUIRE(castle);
    CHECK(castle->gameplay.kind == kGruntKind);
    CHECK(castle->gameplay.stageHealth == std::array<s32, 3>{10, 10, 10});
    CHECK(castle->sequences[3] == "GENERATOR:GGR3");
    CHECK_FALSE(legendsN64Generator(fixture, bytes, "unknown", {}, &warning));
    CHECK_FALSE(warning.empty());
    bytes[4 + 3 * 56 + 9] = 10;
    CHECK_FALSE(legendsN64Generator(fixture, bytes, "boss1", {}, &warning));
    CHECK(warning.find("special") != std::string::npos);
}

TEST_CASE("Native generator libraries follow only the stage's own medium and small species",
          "[legends][legends-generators]") {
    CHECK(legendsN64GeneratorArchives("town1") == std::vector<std::string>{"zom", "mag"});
    CHECK(legendsN64GeneratorArchives("TOWN4") == std::vector<std::string>{"plague", "mag"});
    CHECK(legendsN64GeneratorArchives("castle6") == std::vector<std::string>{"gru", "rat"});
    CHECK(legendsN64GeneratorArchives("castle3") == std::vector<std::string>{"kni", "rat"});
    CHECK(legendsN64GeneratorArchives("castle4") == std::vector<std::string>{"kni", "rat"});
    CHECK(legendsN64GeneratorArchives("battle1") == std::vector<std::string>{"skel", "sked"});
    CHECK(legendsN64GeneratorArchives("ice1").empty());
    CHECK(legendsN64GeneratorArchives("mount1").empty());
    CHECK(legendsN64GeneratorArchives("boss1").empty());
    CHECK(legendsN64GeneratorArchives("unverified").empty());
}

TEST_CASE("N64 generator adapters reject malformed and unsupported native records",
          "[legends][legends-generators]") {
    auto bytes = nativeTemplates();
    auto fixture = nativeFixture();
    std::string warning;
    SECTION("truncated table") {
        bytes.pop_back();
        CHECK_THROWS_AS(legendsN64Generator(fixture, bytes, "ice1", {}), FormatError);
    }
    SECTION("missing native identity") {
        fixture.templateKey = 0x06fe;
        CHECK_THROWS_AS(legendsN64Generator(fixture, bytes, "ice1", {}), FormatError);
    }
    SECTION("nonlinear damage states") {
        bytes[4 + 7 * 56 + 11] = 1;
        CHECK_FALSE(legendsN64Generator(fixture, bytes, "ice1", {}, &warning));
        CHECK(warning.find("non-linear") != std::string::npos);
    }
    SECTION("non-finite contact") {
        real(bytes, 4 + 7 * 56 + 32, std::numeric_limits<f32>::quiet_NaN());
        CHECK_FALSE(legendsN64Generator(fixture, bytes, "ice1", {}, &warning));
        CHECK(warning.find("contact") != std::string::npos);
    }
    SECTION("actor placement is not a generator") {
        fixture.actorInsteadOfGenerator = true;
        CHECK_FALSE(legendsN64Generator(fixture, bytes, "ice1", {}, &warning));
        CHECK(warning.find("placed-enemy") != std::string::npos);
    }
    SECTION("already destroyed") {
        fixture.generatorTier = 0;
        CHECK_FALSE(legendsN64Generator(fixture, bytes, "ice1", {}, &warning));
        CHECK(warning.empty());
    }
    SECTION("outside four participants") {
        fixture.minimumPlayers = 5;
        CHECK_FALSE(legendsN64Generator(fixture, bytes, "ice1", {}, &warning));
        CHECK_FALSE(warning.empty());
    }
    SECTION("live state cannot silently become immortal or empty") {
        bytes[4 + 7 * 56 + 8] = 0;
        CHECK_FALSE(legendsN64Generator(fixture, bytes, "ice1", {}, &warning));
        CHECK(warning.find("durability") != std::string::npos);
    }
}

TEST_CASE("N64 generator balance rounds each live state upward without removing one HP states",
          "[legends][legends-generators]") {
    auto bytes = nativeTemplates();
    bytes[4 + 1 * 56 + 8] = 1;
    bytes[4 + 2 * 56 + 8] = 3;
    bytes[4 + 3 * 56 + 8] = 5;
    const auto generator = legendsN64Generator(nativeFixture(), bytes, "mount1", {});
    REQUIRE(generator);
    CHECK(generator->gameplay.stageHealth == std::array<s32, 3>{1, 2, 3});
    CHECK(generator->gameplay.body.hitPoints == 1);
}

TEST_CASE("N64 special portals separate authored damage-state IDs from their mixed brood",
          "[legends][legends-generators][mixed-generators]") {
    auto fixture = nativeFixture();
    fixture.templateKey = 0x0663;
    fixture.generatorTier = 1;
    auto bytes = mixedTemplates();
    const auto temple = legendsN64Generator(fixture, bytes, "boss1", {});
    REQUIRE(temple);
    CHECK(temple->gameplay.kind == 2);
    CHECK(temple->gameplay.tier == 2);
    CHECK(temple->gameplay.broodTier == 3);
    CHECK(temple->gameplay.broodKinds == std::array<s32, 3>{2, 5, 13});
    CHECK(temple->gameplay.stageHealth == std::array<s32, 3>{21, 21, 0});
    CHECK(temple->sequences ==
          std::array<std::string, 4>{"GENERATOR:GSP0", "GENERATOR:GSP2", "GENERATOR:GSP1", ""});
    const auto hell = legendsN64Generator(fixture, bytes, "hell1", {});
    REQUIRE(hell);
    CHECK(hell->gameplay.broodKinds == std::array<s32, 3>{2, 19, 13});
    CHECK(hell->gameplay.stageHealth == std::array<s32, 3>{25, 25, 0});
    fixture.generatorTier = 2;
    const auto damaged = legendsN64Generator(fixture, bytes, "boss1", {});
    REQUIRE(damaged);
    CHECK(damaged->gameplay.tier == 1);
    CHECK(damaged->gameplay.broodTier == 3);
    CHECK(damaged->gameplay.stageHealth == std::array<s32, 3>{21, 0, 0});
    bytes[4 + 2 * 56 + 11] = 1;
    std::string reason;
    CHECK_FALSE(legendsN64Generator(fixture, bytes, "boss1", {}, &reason));
    CHECK_FALSE(reason.empty()); // A cyclic native chain cannot allocate unbounded states.
}

TEST_CASE("Native generator artwork follows common damage without runtime resource creation",
          "[legends][legends-generators][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/mount1";
    if (!std::filesystem::is_directory(directory)) {
        SKIP("native N64 extraction is not installed");
    }
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                             readFile(directory / "objects.wad"));
    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
    REQUIRE(adapter->generators().size() >= 5);
    for (const auto& warning : adapter->warnings()) {
        INFO(warning);
        CHECK_FALSE(warning.starts_with("Native generator damage artwork"));
    }
    const auto draw = [&] {
        device.draws.clear();
        adapter->draw(device, Mat4{1}, CameraFrame::of(WorldCamera{}), TreeModel::Pass::All);
        return device.draws;
    };
    const auto same = [](const auto& left, const auto& right) {
        return std::ranges::equal(left, right, [](const auto& a, const auto& b) {
            return a.texture == b.texture && a.vertices == b.vertices && a.transform == b.transform;
        });
    };
    const auto survey = draw();
    REQUIRE_FALSE(survey.empty());
    std::vector<ImportedGeneratorState> states;
    for (const auto& generator : adapter->generators()) {
        REQUIRE(generator.kind == 1);
        states.push_back({generator.nativeIndex, -1, 0, {}, false});
    }
    adapter->updateGenerators(states);
    CHECK(same(draw(), survey)); // failed installs must not remove the native figure
    for (usize i = 0; i < states.size(); ++i) {
        const auto& generator = adapter->generators()[i];
        states[i] = {generator.nativeIndex, static_cast<s32>(i), generator.tier, generator.position,
                     true};
    }
    adapter->updateGenerators(states);
    const auto healthy = draw();
    const auto textures = device.texturesCreated;
    const auto fixtureCount = adapter->fixtureCount();
    bool changedTier = false;
    for (auto& state : states) {
        if (state.state > 1) {
            --state.state;
            changedTier = true;
        }
    }
    REQUIRE(changedTier);
    adapter->updateGenerators(states);
    const auto damaged = draw();
    CHECK_FALSE(same(damaged, healthy));
    for (auto& state : states) {
        state.state = 0;
        state.standing = false;
    }
    adapter->updateGenerators(states);
    CHECK_FALSE(same(draw(), damaged));
    CHECK(device.texturesCreated == textures);
    CHECK(adapter->fixtureCount() == fixtureCount);
    const auto destroyed = draw();
    adapter->updateGenerators(states);
    CHECK(same(draw(), destroyed)); // repeated sync never restarts a destruction pose
}

class GeneratorLevel final : public ImportedLevel {
public:
    std::vector<ImportedGenerator> roster;
    void capturePresentation() override {}
    void update(f32 /*seconds*/, bool /*timeStopped*/) override {}
    void draw(RenderDevice& /*device*/, const Mat4& /*clip*/, const WorldCamera& /*camera*/,
              TreeModel::Pass /*pass*/, f32 /*presentationAlpha*/) const override {}
    std::span<const ImportedGenerator> importedGenerators() const override { return roster; }
};

WorldCollision floorCollision() {
    WorldCollision collision;
    CollisionTriangle floor;
    floor.normal = {0, 1, 0};
    floor.vertices = {{{-100, 0, -100}, {0, 0, 100}, {100, 0, -100}}};
    floor.objectFlags = 4;
    collision.build({floor});
    return collision;
}

TEST_CASE("Imported native generators breed attackable common enemies and award destruction",
          "[legends][legends-generators][assets]") {
    const auto objects = test::assetOrSkip("MONSTERS/GRU/objects.ngc");
    const auto root = objects.parent_path().parent_path().parent_path();
    auto definition = legendsN64Generator(nativeFixture(), nativeTemplates(), "castle1", {});
    REQUIRE(definition);
    definition->gameplay.position = {0, 0, 0};
    definition->gameplay.minimumPlayers = 1;
    auto imported = std::make_shared<GeneratorLevel>();
    imported->roster.push_back(definition->gameplay);
    auto invalid = definition->gameplay;
    invalid.nativeIndex = 72;
    invalid.kind = -99;
    imported->roster.push_back(invalid);
    test::FakeRenderDevice device;
    LevelWorld world;
    LevelRef ref;
    ref.realmId = -1;
    ref.name = "n64:generator-test";
    REQUIRE(world.loadImported(device, root, ref,
                               WorldLayout::fromDecoded({}, {}, {-100, 0, -100}, {100, 20, 100}),
                               floorCollision(), imported));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> party;
    opponents.open({device, world, weapons, effects, audio, root}, party);
    CHECK(world.level() == nullptr);
    REQUIRE(opponents.levelNow() != nullptr);
    CHECK(opponents.levelNow()->bossType == -1);
    const auto& feedbackRoster = opponents.levelNow()->enemies;
    REQUIRE(feedbackRoster.size() == 1);
    CHECK(feedbackRoster[0].kind == kGruntKind);
    for (const bool killed : {false, true}) {
        EnemyFeedback feedback;
        feedback.kind = kGruntKind;
        feedback.tier = 3;
        feedback.killed = killed;
        const auto sound = feedback.sound(feedbackRoster);
        REQUIRE_FALSE(sound.empty());
        CHECK(audio.hasNamed(sound));
    }
    auto states = opponents.importedGeneratorStates();
    REQUIRE(states.size() == 2);
    REQUIRE(states[0].pool == 0);
    CHECK(states[0].nativeIndex == 71);
    CHECK(states[0].standing);
    CHECK(states[0].state == 3);
    CHECK(states[1].pool == -1);
    auto& generators = opponents.generators();
    auto& enemies = opponents.enemies();
    REQUIRE(generators.count() == 1);
    CHECK(generators.externallyPresented(0));
    CHECK(generators.presentation().empty());
    generators.draw(device, Mat4{1}, WorldLighting{});
    CHECK(device.draws.empty());
    REQUIRE(generators.obstacles().size() == 1);
    CHECK(generators.struckBy({0, 2, -8}, {0, 2, 8}, 1) == 0);
    const std::array views{EnemyView{.player = 0, .position = {0, 0, 20}}};
    generators.update(2, enemies, views, {}, true);
    CHECK(enemies.count() == 0);
    generators.update(2, enemies, views);
    REQUIRE(enemies.count() == 1);
    REQUIRE(enemies.alive(0));
    CHECK(enemies.kindOf(0) == kGruntKind);
    CHECK(enemies.generatorOf(0) == 0);
    CHECK(generators.livingOf(0) == 1);
    CHECK_FALSE(enemies.targets().empty());
    REQUIRE(world.collision().floorAt(enemies.positionOf(0), 1, 1));
    enemies.draw(device, Mat4{1}, WorldLighting{});
    CHECK_FALSE(device.draws.empty());
    opponents.strikeEnemy(0, 10000, 0, {0, 0, 1}, 0, party);
    CHECK_FALSE(enemies.alive(0));
    CHECK(enemies.dying(0));
    std::string generatorSound;
    // Each calibrated Castle state has ten HP. Eleven damage removes the top
    // stage and carries one point into the next, without skipping two stages.
    CHECK(generators.healthOf(0) == 30);
    REQUIRE(opponents.strikeGenerator(
        0, 11, 0, party, [&](const GeneratorFeedbackEmission& emission) -> std::optional<u32> {
            generatorSound = emission.sound;
            return 0;
        }));
    REQUIRE_FALSE(generatorSound.empty());
    CHECK(audio.hasNamed(generatorSound));
    states = opponents.importedGeneratorStates();
    REQUIRE(states[0].state == 2);
    CHECK(generators.healthOf(0) == 19);
    opponents.strikeGenerator(0, 10000, 0, party);
    states = opponents.importedGeneratorStates();
    CHECK_FALSE(states[0].standing);
    CHECK(states[0].state == 0);
    CHECK(generators.obstacles().empty());
    const s32 bred = generators.bredOf(0);
    generators.update(600, enemies, views);
    CHECK(generators.bredOf(0) == bred);
    s32 earned = 0;
    s32 destroyed = 0;
    LevelOpponents::Events events;
    events.levels = [] {};
    events.award = [&](s32 player, s32 experience, bool) {
        CHECK(player == 0);
        earned += experience;
    };
    events.destroyedGenerator = [&](s32 player) {
        CHECK(player == 0);
        ++destroyed;
    };
    opponents.settleRewards(party, events);
    CHECK(earned > 0);
    CHECK(destroyed == 1);
    opponents.close();
    CHECK(opponents.importedGeneratorStates().empty());
    CHECK_FALSE(generators.externallyPresented(0));
}

TEST_CASE("Imported durability preserves unequal stages excess damage and ordinary generators",
          "[legends][legends-generators][assets]") {
    const auto root =
        test::assetOrSkip("MONSTERS/GRU/objects.ngc").parent_path().parent_path().parent_path();
    auto definition = legendsN64Generator(nativeFixture(), nativeTemplates(), "castle1", {});
    REQUIRE(definition);
    auto& authored = definition->gameplay;
    authored.minimumPlayers = 0;
    authored.stageHealth = {7, 11, 19};
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, nullptr, 4, {}, 1);
    Generators generators;
    REQUIRE(generators.bind(device, {}, enemies, nullptr, {}, 1));
    const auto slot = generators.spawnImported(device, enemies, authored);
    REQUIRE(slot);
    CHECK(generators.healthOf(*slot) == 37);
    Generators::LocalSnapshot before;
    generators.saveLocal(before);
    for (const auto [damage, health, state] :
         std::array{std::array{18, 19, 3}, std::array{1, 18, 2}, std::array{10, 8, 2},
                    std::array{1, 7, 1}, std::array{7, 0, 0}}) {
        REQUIRE(generators.strike(*slot, static_cast<f32>(damage), 0));
        CHECK(generators.healthOf(*slot) == health);
        CHECK(generators.stateOf(*slot) == state);
    }
    REQUIRE(generators.restoreLocal(before));
    REQUIRE(generators.strike(*slot, 31, 0));
    CHECK(generators.healthOf(*slot) == 6);
    CHECK(generators.stateOf(*slot) == 1); // one strike carries through both broken states
    const auto ordinary = generators.spawn(device, enemies, kGruntKind, 3, {30, 0, 0}, 0);
    REQUIRE(ordinary);
    CHECK(generators.healthOf(*ordinary) == 300);
    REQUIRE(generators.strike(*ordinary, 100, 0));
    CHECK(generators.healthOf(*ordinary) == 200);
    CHECK(generators.stateOf(*ordinary) == 2);

    for (const auto health : {std::array<s32, 3>{-1, 11, 19}, {0, 11, 19}, {7, 100001, 19}}) {
        auto bad = authored;
        bad.stageHealth = health;
        CHECK_FALSE(generators.spawnImported(device, enemies, bad));
    }
    auto badTier = authored;
    badTier.tier = 1;
    CHECK_FALSE(generators.spawnImported(device, enemies, badTier));
    generators.clear();
    REQUIRE(generators.bind(device, {}, enemies, nullptr, {}, 1));
    const auto reused = generators.spawn(device, enemies, kGruntKind, 3, {}, 0);
    REQUIRE(reused == 0);
    REQUIRE(generators.strike(*reused, 19, 0));
    CHECK(generators.healthOf(*reused) == 281);
    CHECK(generators.stateOf(*reused) == 3); // no stale imported thresholds after level close
}

TEST_CASE("N64 generator balance halves native realm-scaled PGEn stages for GDL combat",
          "[legends][legends-generators][assets]") {
    const auto root = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::exists(root / "_program/game.bin")) {
        SKIP("Native N64 Legends extraction is not installed");
    }
    const auto program = readFile(root / "_program/game.bin");
    const auto templates = readFile(root / "items/generic/pgen.rom");
    const auto stages = LegendsN64Unlocks::decode(program);
    usize checked = 0;
    for (const auto& stage : stages.stages()) {
        const auto generator = legendsN64Generator(nativeFixture(), templates, stage.map, {});
        if (!generator) {
            continue;
        }
        CAPTURE(stage.map);
        const auto nativeHealth = nativeHealthOf(*generator, templates, program, stage);
        for (usize state = 1; state <= 3; ++state) {
            CHECK(generator->gameplay.stageHealth[state - 1] == (nativeHealth[state - 1] + 1) / 2);
        }
        if (stage.map == "mount1") {
            CHECK(nativeHealth == std::array<s32, 3>{18, 18, 24});
            CHECK(generator->gameplay.stageHealth == std::array<s32, 3>{9, 9, 12});
        }
        if (stage.map == "mount4") {
            CHECK(nativeHealth == std::array<s32, 3>{20, 22, 26});
            CHECK(generator->gameplay.stageHealth == std::array<s32, 3>{10, 11, 13});
        }
        ++checked;
    }
    CHECK(checked == 21);
}

TEST_CASE("Native N64 mountain generators use real collision for common enemy births",
          "[legends][legends-generators][assets]") {
    const auto directory = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::exists(directory / "levels/mount1/ipr.rom")) {
        SKIP("native N64 Legends extraction is not installed");
    }
    const auto objects = test::assetOrSkip("MONSTERS/TRO/objects.ngc");
    const auto root = objects.parent_path().parent_path().parent_path();
    const auto generic = directory / "items/generic";
    const auto pgen = readFile(generic / "pgen.rom");
    const auto ppow = readFile(generic / "ppow.rom");
    const auto pdec = readFile(generic / "pdec.rom");
    const auto ptrap = readFile(generic / "ptrap.rom");
    const auto ptel = readFile(generic / "ptel.rom");
    const auto pcon = readFile(generic / "pcon.rom");
    const auto prnd = readFile(generic / "prnd.rom");
    const formats::LegendsN64Templates templates{ppow, pdec, pgen, ptrap, ptel, pcon, prnd};
    const auto level = directory / "levels/mount1";
    const auto fixtures = formats::decodeLegendsN64Fixtures(readFile(level / "ipr.rom"), templates);
    const auto map = formats::LegendsMap::parseN64(readFile(level / "worlds.wad"),
                                                   readFile(level / "objects.wad"));
    LegendsTraversal traversal;
    traversal.open(map, 0, CharacterSave{});
    test::FakeRenderDevice device;
    Enemies enemies;
    enemies.open(device, root, &traversal.collision(), Enemies::kMost, {}, 1);
    Generators generators;
    REQUIRE(generators.bind(device, WorldLayout{}, enemies, &traversal.collision(), {}, 1));
    std::vector<ImportedGenerator> installed;
    for (const auto& fixture : fixtures) {
        if (fixture.minimumPlayers > 1) {
            continue;
        }
        const auto generator = legendsN64Generator(fixture, pgen, "mount1", map.collision);
        if (!generator) {
            continue;
        }
        CHECK(generator->gameplay.kind == 1);
        REQUIRE(generators.spawnImported(device, enemies, generator->gameplay));
        installed.push_back(generator->gameplay);
    }
    REQUIRE(installed.size() >= 5);
    for (const auto& generator : installed) {
        const std::array views{
            EnemyView{.player = 0, .position = generator.position + Vec3{0, 0, 15}}};
        generators.update(2, enemies, views);
    }
    REQUIRE(enemies.count() > 0);
    for (s32 id = 0; id < Enemies::kMost; ++id) {
        if (!enemies.alive(id)) {
            continue;
        }
        CHECK(enemies.kindOf(id) == 1);
        CHECK(enemies.generatorOf(id) >= 0);
        const auto floor = traversal.collision().floorAt(enemies.positionOf(id), 1, 1);
        REQUIRE(floor);
        CHECK(enemies.positionOf(id).y == Approx(floor->y).margin(.2f));
    }
    enemies.draw(device, Mat4{1}, WorldLighting{});
    CHECK_FALSE(device.draws.empty());
}

TEST_CASE("Desecrated Temple generators install all native portals and breed three real species",
          "[legends][legends-generators][mixed-generators][assets]") {
    const auto directory =
        test::dataDirectory().parent_path() / "assets/n64/extracted/levels/boss1";
    if (!std::filesystem::exists(directory / "ipr.rom")) {
        SKIP("Native N64 Legends extraction is not installed");
    }
    const auto root =
        test::assetOrSkip("MONSTERS/DEM/objects.ngc").parent_path().parent_path().parent_path();
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                             readFile(directory / "objects.wad"),
                                             readFile(directory / "ipr.rom"));
    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
    REQUIRE(adapter->generators().size() == 34);
    auto multiplayer = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map, 4);
    CHECK(multiplayer->generators().size() == 72);
    for (const auto& definition : adapter->generators()) {
        CHECK(definition.tier == 2);
        CHECK(definition.stageHealth == std::array<s32, 3>{21, 21, 0});
        CHECK(definition.broodKinds == std::array<s32, 3>{2, 5, 13});
    }
    LegendsTraversal traversal;
    traversal.open(map, 0, CharacterSave{});
    auto imported = std::make_shared<GeneratorLevel>();
    imported->roster.assign(adapter->generators().begin(), adapter->generators().end());
    LevelWorld world;
    LevelRef ref;
    ref.realmId = -1;
    ref.name = "n64:boss1";
    REQUIRE(world.loadImported(device, root, ref,
                               WorldLayout::fromDecoded({}, {}, map.minBounds, map.maxBounds),
                               traversal.collision(), imported));
    ItemArchive weapons;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    std::array<PlayerRuntime, 1> party;
    opponents.open({device, world, weapons, effects, audio, root}, party);
    auto& generators = opponents.generators();
    auto& enemies = opponents.enemies();
    REQUIRE(generators.count() == 34);
    const auto installed = opponents.importedGeneratorStates();
    REQUIRE(std::ranges::all_of(installed, [](const auto& state) {
        return state.pool >= 0 && state.standing && state.state == 2;
    }));
    adapter->updateGenerators(installed);
    const auto initialResources = device.texturesCreated;
    REQUIRE(opponents.levelNow());
    for (const auto kind : {2, 5, 13}) {
        CHECK(std::ranges::any_of(opponents.levelNow()->enemies,
                                  [&](const auto& entry) { return entry.kind == kind; }));
    }
    const std::array views{
        EnemyView{.player = 0, .position = generators.positionOf(0) + Vec3{0, 0, 15}}};
    Generators::LocalSnapshot before;
    generators.saveLocal(before);
    generators.update(120, enemies, views, {}, true);
    CHECK(enemies.count() == 0);
    std::set<s32> seen;
    for (s32 attempt = 0; attempt < 60 && seen.size() != 3; ++attempt) {
        generators.update(120, enemies, views);
        for (s32 slot = 0; slot < Enemies::kMost; ++slot) {
            if (!enemies.alive(slot) || enemies.dying(slot)) {
                continue;
            }
            seen.insert(enemies.kindOf(slot));
            CHECK(enemies.tierOf(slot) == 3);
            CHECK(enemies.generatorOf(slot) >= 0);
            REQUIRE(world.collision().floorAt(enemies.positionOf(slot), 1, 1));
            opponents.strikeEnemy(slot, 10000, 0, {0, 0, 1}, 0, party);
        }
        enemies.update(120, 2, views);
    }
    CHECK(seen == std::set<s32>{2, 5, 13});
    REQUIRE(generators.strike(0, 21, 0));
    CHECK(generators.stateOf(0) == 1);
    CHECK(generators.healthOf(0) == 21);
    adapter->updateGenerators(opponents.importedGeneratorStates());
    REQUIRE(generators.strike(0, 21, 0));
    CHECK_FALSE(generators.standing(0));
    CHECK(generators.stateOf(0) == 0);
    adapter->updateGenerators(opponents.importedGeneratorStates());
    CHECK(device.texturesCreated == initialResources);
    REQUIRE(generators.restoreLocal(before));
    Generators::LocalSnapshot restored;
    generators.saveLocal(restored);
    CHECK(generators.equalLocal(before, restored));
    opponents.close();
}

TEST_CASE("Desecrated Temple authored portal receives weapons through its actual native collision",
          "[legends][legends-generators][native-generator-placement][assets]") {
    const auto native = test::dataDirectory().parent_path() / "assets/n64/extracted";
    const auto directory = native / "levels/boss1";
    if (!std::filesystem::is_regular_file(directory / "worlds.wad"))
        SKIP("N64 native extraction is not installed");
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    auto map = formats::LegendsMap::parseN64(readFile(directory / "worlds.wad"),
                                             readFile(directory / "objects.wad"),
                                             readFile(directory / "ipr.rom"));
    test::FakeRenderDevice device;
    auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, directory, map);
    REQUIRE(adapter->generators().size() == 34);
    auto imported = std::make_shared<GeneratorLevel>();
    imported->roster.assign(adapter->generators().begin(), adapter->generators().end());
    LegendsTraversal traversal;
    traversal.open(map, 0, CharacterSave{});
    LevelWorld world;
    LevelRef ref;
    ref.realmId = -1;
    ref.name = "n64:boss1";
    REQUIRE(world.loadImported(device, root, ref,
                               WorldLayout::fromDecoded({}, {}, map.minBounds, map.maxBounds),
                               traversal.collision(), imported));
    ClassDataSet classes;
    REQUIRE(classes.load(root / "pdata"));
    ItemArchive weapons;
    REQUIRE(weapons.load(root / "WEAPONS"));
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    LevelFixtures fixtures;
    std::array<PlayerRuntime, 1> players;
    opponents.open({device, world, weapons, effects, audio, root}, players);
    auto& generators = opponents.generators();
    REQUIRE(generators.count() == 34);
    Generators::LocalSnapshot initial;
    generators.saveLocal(initial);
    PlayerArsenal arsenal;
    AmbientDimmer dimmer;
    PlayerAttacks attacks;
    attacks.bind({device, classes, world, weapons, effects, audio, nullptr, arsenal, dimmer});
    PlayerAttacks::Targets targets{opponents, fixtures, {}, MultiplayerMode::Normal, players};
    const auto& placement = imported->roster.front();
    const Vec3 outward{std::sin(placement.yaw), 0, std::cos(placement.yaw)};
    const Vec3 center = placement.position + Vec3{0, placement.body.height * .5f, 0};
    const f32 depth = std::max(
        {placement.body.radius, generators.boxOf(0).halfAlong, generators.boxOf(0).halfAcross});
    for (const s32 level : {1, 99}) {
        for (const bool ranged : {false, true}) {
            CAPTURE(level, ranged, placement.nativeIndex, placement.position.x,
                    placement.position.y, placement.position.z, placement.yaw);
            REQUIRE(generators.restoreLocal(initial));
            CharacterSave save;
            save.progress().experience = levelExperience(level);
            Vec3 feet = placement.position + outward * (depth + (ranged ? 12.0f : 1.0f));
            const auto floor = world.collision().floorAt(feet, 3, 3);
            REQUIRE(floor);
            feet.y = floor->y;
            players[0].actor.spawn(0, save, classes.stats(0), feet, 0);
            players[0].actor.faceToward(center);
            players[0].figure = PlayerFigure::load(device, root, save, false);
            REQUIRE(players[0].figure);
            arsenal.bind(
                {device, classes, weapons, world.collision(), effects, audio, nullptr, {}});
            const auto shot = arsenal.weaponThrow(players[0].actor, 0);
            REQUIRE(shot);
            const auto perHit = static_cast<s32>(
                std::round(ranged ? shot->damage : attacks.meleeDamageOf(0, players)));
            // Retail has two 42-HP states; the explicit GDL balance policy
            // retains both states at 21 HP, without altering weapon damage.
            const auto expected = (42 + perHit - 1) / perHit;
            s32 hits = 0;
            while (generators.standing(0) && hits <= expected) {
                const auto before = generators.healthOf(0);
                if (ranged) {
                    arsenal.clear();
                    arsenal.bind(
                        {device, classes, weapons, world.collision(), effects, audio, nullptr, {}});
                    arsenal.launchWeapon(players[0].actor, players[0].figure.get(), -outward, 1,
                                         false, center);
                    for (s32 tick = 0; tick < 120 && generators.healthOf(0) == before; ++tick)
                        attacks.updateProjectiles(1.0f / 60, players, targets);
                } else {
                    attacks.melee(0, players, targets);
                }
                REQUIRE(generators.healthOf(0) < before);
                ++hits;
            }
            CHECK_FALSE(generators.standing(0));
            CHECK(hits == expected);
        }
    }
}

TEST_CASE("Every authored N64 generator damage chain accepts real level-one and level-99 melee",
          "[legends][legends-generators][native-generator-weapons][assets]") {
    const auto native = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(native / "_program/game.bin"))
        SKIP("N64 native extraction is not installed");
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    const auto generic = native / "items/generic";
    const auto ppow = readFile(generic / "ppow.rom");
    const auto pdec = readFile(generic / "pdec.rom");
    const auto pgen = readFile(generic / "pgen.rom");
    const auto ptrap = readFile(generic / "ptrap.rom");
    const auto ptel = readFile(generic / "ptel.rom");
    const auto pcon = readFile(generic / "pcon.rom");
    const auto prnd = readFile(generic / "prnd.rom");
    const auto program = readFile(native / "_program/game.bin");
    const formats::LegendsN64Templates templates{ppow, pdec, pgen, ptrap, ptel, pcon, prnd};
    const auto stages = LegendsN64Unlocks::decode(program);
    std::vector<std::tuple<std::string, ImportedGenerator, std::array<s32, 3>>> roster;
    usize maps = 0;
    for (const auto& entry : std::filesystem::directory_iterator(native / "levels")) {
        if (!std::filesystem::is_regular_file(entry.path() / "worlds.wad"))
            continue;
        ++maps;
        const auto name = entry.path().filename().string();
        for (const auto& fixture :
             formats::decodeLegendsN64Fixtures(readFile(entry.path() / "ipr.rom"), templates)) {
            if (const auto definition = legendsN64Generator(fixture, pgen, name, {})) {
                const auto stage =
                    std::ranges::find(stages.stages(), name, &LegendsStageUnlock::map);
                REQUIRE(stage != stages.stages().end());
                const auto nativeHealth = nativeHealthOf(*definition, pgen, program, *stage);
                if (name == "boss1" && definition->gameplay.tier == 2)
                    CHECK(nativeHealth == std::array<s32, 3>{42, 42, 0});
                if (name == "hell1" && definition->gameplay.tier == 2)
                    CHECK(nativeHealth == std::array<s32, 3>{49, 49, 0});
                roster.emplace_back(name, definition->gameplay, nativeHealth);
            }
        }
    }
    REQUIRE(maps == 34);
    REQUIRE(roster.size() > 300);
    test::FakeRenderDevice device;
    ClassDataSet classes;
    REQUIRE(classes.load(root / "pdata"));
    LevelWorld world;
    LevelRef ref;
    ref.realmId = -1;
    ref.name = "n64:generator-weapon-census";
    REQUIRE(world.loadImported(device, root, ref,
                               WorldLayout::fromDecoded({}, {}, {-100, 0, -100}, {100, 30, 100}),
                               floorCollision(), std::make_shared<GeneratorLevel>()));
    ItemArchive weapons;
    REQUIRE(weapons.load(root / "WEAPONS"));
    EffectTrees effects;
    LevelSoundscape audio;
    LevelOpponents opponents;
    LevelFixtures fixtures;
    std::array<PlayerRuntime, 1> players;
    opponents.open({device, world, weapons, effects, audio, root}, players);
    REQUIRE(opponents.enemies().setArchiveLoader([&](s32 kind) {
        std::vector<std::string> warnings;
        return loadLegendsEnemyAssets(native, kind, program, warnings);
    }));
    PlayerArsenal arsenal;
    arsenal.bind({device, classes, weapons, world.collision(), effects, audio, nullptr, {}});
    AmbientDimmer dimmer;
    PlayerAttacks attacks;
    attacks.bind({device, classes, world, weapons, effects, audio, nullptr, arsenal, dimmer});
    PlayerAttacks::Targets targets{opponents, fixtures, {}, MultiplayerMode::Normal, players};
    auto& generators = opponents.generators();
    for (s32 character = 0; character < 4; ++character) {
        for (s32 level : {1, 99}) {
            CharacterSave save;
            save.character = character;
            save.progress().experience = levelExperience(level);
            players[0].actor.spawn(0, save, classes.stats(character), {0, 0, -4}, 0);
            players[0].figure = PlayerFigure::load(device, root, save, false);
            REQUIRE(players[0].figure);
            const auto damage = attacks.meleeDamageOf(0, players);
            REQUIRE(damage > 1);
            std::set<std::tuple<std::string, s32, s32, std::array<s32, 3>>> shotCases;
            s32 smallestMelee = std::numeric_limits<s32>::max(), largestMelee = 0;
            s32 smallestShots = std::numeric_limits<s32>::max(), largestShots = 0;
            s32 portalMelee = 0, portalShots = 0;
            for (const auto& [name, original, nativeHealth] : roster) {
                CAPTURE(name, original.nativeIndex, original.kind, original.tier, character, level,
                        damage);
                auto definition = original;
                definition.position = {};
                definition.yaw = 0;
                generators.clear();
                // The corpus includes every placement, including those reserved
                // for larger parties. Admit four seats without rewriting source
                // placement thresholds; player zero performs the weapon test.
                REQUIRE(
                    generators.bind(device, {}, opponents.enemies(), &world.collision(), {}, 4));
                const auto id = generators.spawnImported(device, opponents.enemies(), definition);
                REQUIRE(id);
                REQUIRE(generators.standing(*id));
                s32 health = 0;
                for (usize state = 0; state < nativeHealth.size(); ++state) {
                    const auto calibrated = (nativeHealth[state] + 1) / 2;
                    CHECK(original.stageHealth[state] == calibrated);
                    health += calibrated;
                }
                REQUIRE(generators.healthOf(*id) == health);
                const auto& box = generators.boxOf(*id);
                const f32 depth = std::max({box.halfAlong, box.halfAcross, definition.body.radius});
                players[0].actor.place({0, 0, -depth - 1});
                s32 count = 0;
                const auto perHit = static_cast<s32>(std::round(damage));
                const auto expected = (health + perHit - 1) / perHit;
                while (generators.standing(*id) && count <= expected) {
                    attacks.melee(0, players, targets);
                    ++count;
                }
                CHECK_FALSE(generators.standing(*id));
                CHECK(count == expected);
                smallestMelee = std::min(smallestMelee, count);
                largestMelee = std::max(largestMelee, count);
                if (name == "boss1")
                    portalMelee = count;
                // Exercise each map's distinct damage chain through a real flying
                // weapon too. Repeated placements keep the exhaustive melee check
                // above, without redundantly simulating identical missile flights.
                if (shotCases.emplace(name, original.kind, original.tier, original.stageHealth)
                        .second) {
                    generators.clear();
                    REQUIRE(generators.bind(device, {}, opponents.enemies(), &world.collision(), {},
                                            4));
                    const auto shotId =
                        generators.spawnImported(device, opponents.enemies(), definition);
                    REQUIRE(shotId);
                    REQUIRE(generators.standing(*shotId));
                    players[0].actor.place({0, 0, -depth - 12});
                    const auto shot = arsenal.weaponThrow(players[0].actor, 0);
                    REQUIRE(shot);
                    const auto shotDamage = static_cast<s32>(std::round(shot->damage));
                    const auto expectedShots = (health + shotDamage - 1) / shotDamage;
                    s32 shots = 0;
                    while (generators.standing(*shotId) && shots <= expectedShots) {
                        arsenal.clear();
                        arsenal.bind({device,
                                      classes,
                                      weapons,
                                      world.collision(),
                                      effects,
                                      audio,
                                      nullptr,
                                      {}});
                        const auto before = generators.healthOf(*shotId);
                        arsenal.launchWeapon(players[0].actor, players[0].figure.get(), {0, 0, 1},
                                             1, false, Vec3{0, definition.body.height * .5f, 0});
                        for (s32 tick = 0; tick < 120 && generators.healthOf(*shotId) == before;
                             ++tick)
                            attacks.updateProjectiles(1.0f / 60, players, targets);
                        REQUIRE(generators.healthOf(*shotId) < before);
                        ++shots;
                    }
                    CHECK_FALSE(generators.standing(*shotId));
                    CHECK(shots == expectedShots);
                    smallestShots = std::min(smallestShots, shots);
                    largestShots = std::max(largestShots, shots);
                    if (name == "boss1")
                        portalShots = shots;
                }
            }
            WARN("Native generator weapon census: class="
                 << character << " level=" << level << " placements=" << roster.size()
                 << " melee=" << smallestMelee << ".." << largestMelee
                 << " projectile=" << smallestShots << ".." << largestShots
                 << " boss1=" << portalMelee << " swings / " << portalShots << " throws");
        }
    }
}

TEST_CASE("Every native generator placement reaches the real scene and common population pool",
          "[legends][legends-generators][native-generator-population][assets]") {
    const auto nativeRoot = test::dataDirectory().parent_path() / "assets/n64/extracted";
    if (!std::filesystem::is_regular_file(nativeRoot / "_program/game.bin"))
        SKIP("Native N64 extraction is not installed");
    const auto root = test::assetOrSkip("WEAPONS/ANIM.PS2").parent_path().parent_path();
    usize maps = 0;
    usize allPlayers = 0;
    for (const auto& entry : std::filesystem::directory_iterator(nativeRoot / "levels")) {
        if (!std::filesystem::is_regular_file(entry.path() / "worlds.wad"))
            continue;
        ++maps;
        const auto name = entry.path().filename().string();
        const auto ipr = readFile(entry.path() / "ipr.rom");
        const usize first = readU32BE(ipr, 8);
        const usize count = readU16BE(ipr, 40);
        REQUIRE(first <= ipr.size());
        REQUIRE(count <= (ipr.size() - first) / 16);
        for (const s32 partySize : {1, 4}) {
            CAPTURE(name, partySize);
            // Independent native placement roster: key category6, nonzero
            // state, generator flag rather than placed actor, player threshold.
            // Do not derive expectations from the adapter under test.
            std::set<u32> expected;
            for (usize i = 0; i < count; ++i) {
                const usize at = first + i * 16;
                if (ipr[at + 8] == 6 && ipr[at + 10] != 0 && (ipr[at + 13] & 2U) == 0 &&
                    ipr[at + 14] <= partySize)
                    expected.insert(static_cast<u32>(i));
            }
            if (partySize == 4)
                allPlayers += expected.size();
            if (name.starts_with("town") && name != "town5") {
                constexpr std::array<usize, 4> kSolo{47, 51, 52, 43};
                constexpr std::array<usize, 4> kParty{114, 118, 88, 91};
                const usize stage = static_cast<usize>(name.back() - '1');
                REQUIRE(stage < 4);
                CHECK(expected.size() == (partySize == 1 ? kSolo[stage] : kParty[stage]));
            }
            auto map = formats::LegendsMap::parseN64(readFile(entry.path() / "worlds.wad"),
                                                     readFile(entry.path() / "objects.wad"), ipr);
            test::FakeRenderDevice device;
            auto adapter = LegendsSceneAdapter::load(device, LegendsPlatform::N64, entry.path(),
                                                     map, partySize);
            REQUIRE(adapter);
            std::set<u32> actual;
            for (const auto& generator : adapter->generators()) {
                CHECK(actual.insert(generator.nativeIndex).second);
            }
            for (const auto& warning : adapter->warnings()) {
                INFO(warning);
                CHECK_FALSE(warning.starts_with("Unbound fixture: GENERATOR:"));
                CHECK_FALSE(warning.starts_with("Native generator damage artwork"));
            }
            REQUIRE(actual == expected);
            if (name == "castle4") {
                // The native IPR contains GGR templates, but e0067818 remaps
                // these to GKN before tree creation. No grunt archive belongs
                // to this stage: an initial GGR lookup used to discard all 21
                // solo (39 four-player) knight generators before admission.
                const usize knights = partySize == 1 ? 21 : 39;
                const auto knight = enemyKindOf("KNI");
                REQUIRE(knight);
                CHECK(std::ranges::count(adapter->generators(), *knight,
                                         &ImportedGenerator::kind) == knights);
                usize visibleKnights = 0;
                for (usize i = 0; i < adapter->fixtureCount(); ++i) {
                    const auto object = adapter->fixtureObject(
                        LegendsSceneAdapter::kFixtureObjectBase + static_cast<s32>(i));
                    if (object && object->visible && object->name.starts_with("GENERATOR:GKN"))
                        ++visibleKnights;
                    if (object && object->visible)
                        CHECK_FALSE(object->name.starts_with("GENERATOR:GGR"));
                }
                CHECK(visibleKnights == knights);
            }
            if (expected.empty())
                continue;
            auto native =
                std::make_shared<LegendsLevel>(device, map, std::move(adapter), entry.path(), root);
            LegendsTraversal traversal;
            traversal.open(map, 0, CharacterSave{});
            LevelWorld world;
            LevelRef ref;
            ref.realmId = -1;
            ref.name = "n64:" + name;
            REQUIRE(world.loadImported(
                device, root, ref, WorldLayout::fromDecoded({}, {}, map.minBounds, map.maxBounds),
                traversal.collision(), native));
            ItemArchive weapons;
            EffectTrees effects;
            LevelSoundscape audio;
            std::vector<PlayerRuntime> players(static_cast<usize>(partySize));
            REQUIRE_FALSE(map.starts.empty());
            for (usize i = 0; i < players.size(); ++i)
                players[i].actor.spawn(static_cast<s32>(i), {}, nullptr,
                                       map.starts.front().position, 0);
            LevelOpponents opponents;
            opponents.open({device, world, weapons, effects, audio, root}, players);
            auto& generators = opponents.generators();
            auto& enemies = opponents.enemies();
            REQUIRE(generators.count() == expected.size());
            const auto states = opponents.importedGeneratorStates();
            REQUIRE(states.size() == expected.size());
            for (const auto& state : states) {
                CAPTURE(state.nativeIndex, state.pool);
                REQUIRE(state.pool >= 0);
                CHECK(state.standing);
                CHECK(generators.healthOf(state.pool) > 0);
                CHECK(generators.externallyPresented(state.pool));
            }
            native->updateGenerators(states);
            if (name.starts_with("town")) {
                // Exercise native floor/collision, not the old isolated-floor
                // metadata corpus. Walking the authored posts must birth both
                // stage species, with the native archives actually drawing.
                std::set<s32> wanted;
                for (const auto& generator : native->importedGenerators())
                    wanted.insert(generator.kind);
                std::set<s32> born;
                for (const auto& generator : native->importedGenerators()) {
                    if (born.contains(generator.kind))
                        continue;
                    const std::array view{
                        EnemyView{.player = 0, .position = generator.position + Vec3{0, 0, 15}}};
                    for (s32 attempt = 0; attempt < 3; ++attempt)
                        generators.update(120, enemies, view);
                    for (s32 id = 0; id < Enemies::kMost; ++id) {
                        if (enemies.alive(id) && enemies.generatorOf(id) >= 0)
                            born.insert(enemies.kindOf(id));
                    }
                    if (born == wanted)
                        break;
                }
                CHECK(born == wanted);
                device.draws.clear();
                enemies.draw(device, Mat4{1}, WorldLighting{});
                CHECK_FALSE(device.draws.empty());
                for (const auto kind : born) {
                    REQUIRE(enemies.archive(kind));
                    CHECK(enemies.archive(kind)->loaded());
                }
            }
            opponents.close();
            effects.clear();
        }
    }
    CHECK(maps == 34);
    CHECK(allPlayers == 2072);
}

} // namespace
