#include <array>
#include <cstdint>
#include <filesystem>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "game/screens/BossSequence.h"

namespace {
using namespace gdl;
using namespace gdl::game;

static_assert(!std::is_move_constructible_v<BossSequence>);
static_assert(!std::is_copy_constructible_v<BossSequence>);

struct Fixture {
    test::FakeRenderDevice device;
    LevelWorld world;
    ItemArchive weapons;
    TextureSet textures;
    EffectTrees effects;
    LevelSoundscape audio;
    LevelCatalog levels;
    Bosses bosses;
    BossSequence sequence;
    std::array<PlayerRuntime, 2> players;
    MessageTable strings;
    Fixture() {
        players[0].actor.spawn(3, {}, nullptr, Vec3{1, 2, 3}, 0);
        players[1].actor.spawn(1, {}, nullptr, Vec3{4, 5, 6}, 0);
    }
    void bind() { sequence.bind({device, world, weapons, textures, effects, audio, &levels}); }
    void loadLevel() {
        const auto root = test::scratchDirectory("boss-sequence");
        const auto level = root / "LEVELS/LEVELG5";
        std::filesystem::create_directories(level);
        std::filesystem::create_directories(root / "wdata");
        writeTextFile(root / "wdata/TOWN.json", R"({"realm":7,"prefix":"levelG","levels":[
            {"name":"G5","bossType":41,"rune":1},{"name":"G1","rune":2}]})");
        writeTextFile(level / "world.json", R"({"objects":[
            {"name":"FLOOR","object":0,"parent":-1,"position":[0,0,0]}]})");
        writeTextFile(level / "objects.json",
                      R"({"objects":[{"name":"FLOOR","file":"mesh.obj","meshTriangles":1}]})");
        writeTextFile(level / "mesh.obj",
                      "v 0 0 0\nv 1 0 0\nv 0 0 1\nvt 0 0\nvt 1 0\nvt 0 1\nvn 0 1 0\n"
                      "usemtl tex0\nf 1/1/1 2/2/1 3/3/1\n");
        writeFile(level / "skin.png", test::kTinyPng);
        writeTextFile(level / "textures.json", R"({"bitmaps":[
            {"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
        REQUIRE(levels.load(root));
        const auto ref = levels.byName("G5");
        REQUIRE(ref.has_value());
        REQUIRE(world.load(device, root, *ref));
        REQUIRE(world.level() != nullptr);
        bind();
    }
};

TEST_CASE("boss sequence bearers use player ids and tolerate missing or fallen figures",
          "[game][screens][boss-sequence]") {
    Fixture f;
    REQUIRE_FALSE(BossSequence::bearer(0, 34, f.players).has_value());
    const auto bearer = BossSequence::bearer(1, 34, f.players);
    REQUIRE(bearer.has_value());
    REQUIRE(bearer->player == 1);
    REQUIRE(bearer->position == Vec3{4, 5, 6});
    REQUIRE(bearer->holdPoint == Vec3{4, 5 + LegendShow::kHeldLift, 6});
    REQUIRE_FALSE(bearer->canGesture);
    REQUIRE_FALSE(bearer->casting);
    REQUIRE_FALSE(bearer->released);
    f.players[1].figure = std::make_unique<PlayerFigure>();
    REQUIRE(BossSequence::bearer(1, 34, f.players)->canGesture);
    f.players[1].life = PlayerLife::Dying;
    REQUIRE_FALSE(BossSequence::bearer(1, 34, f.players)->canGesture);
}

TEST_CASE("boss sequence is inert when closed or when no level record exists",
          "[game][screens][boss-sequence]") {
    Fixture f;
    f.sequence.fallen(Vec3{0}, f.bosses, f.players);
    REQUIRE_FALSE(f.sequence.advanceVictory(300, 5, f.players, f.strings));
    f.bind();
    f.sequence.fallen(Vec3{0}, f.bosses, f.players);
    f.sequence.advanceLegend(1, f.bosses, f.players);
    REQUIRE_FALSE(f.sequence.victory().state().running());
    REQUIRE(f.players[0].actor.save().progress().relics.shards == 0);
    f.sequence.clear();
    f.sequence.clear();
    REQUIRE(f.sequence.frozenTexture() == nullptr);
    REQUIRE(f.effects.count() == 0);
}

TEST_CASE("boss victory rewards the entire party once and signals completion once",
          "[game][screens][boss-sequence]") {
    Fixture f;
    f.loadLevel();
    f.players[1].life = PlayerLife::InTower;
    // Level records number runes from one; save bits are zero-based.
    f.players[0].actor.save().progress().relics.addRune(0);
    f.players[1].actor.save().progress().relics.addRune(1);
    f.sequence.fallen(Vec3{0}, f.bosses, f.players);
    REQUIRE(f.sequence.victory().state().stage() == BossVictory::Stage::Waiting);
    REQUIRE(f.sequence.victory().state().runeQuality() == 3);
    for (const auto& player : f.players) {
        REQUIRE(player.actor.save().progress().relics.hasShard(LevelRef::orderOf(7)));
    }
    // A duplicate fall notification must not reset the visit or grant another reward.
    f.sequence.advanceVictory(BossVictory::kWaitTicks, 0, f.players, f.strings);
    REQUIRE(f.sequence.victory().state().stage() == BossVictory::Stage::Appearing);
    f.players[0].actor.save().progress().relics.shards = 0;
    f.sequence.fallen(Vec3{0}, f.bosses, f.players);
    REQUIRE(f.players[0].actor.save().progress().relics.shards == 0);
    REQUIRE(f.sequence.victory().state().stage() == BossVictory::Stage::Appearing);
    std::int32_t completions = 0;
    for (std::int32_t tick = 0; tick < 2000; ++tick) {
        completions += f.sequence.advanceVictory(2, 1.0f / 30.0f, f.players, f.strings) ? 1 : 0;
    }
    REQUIRE(completions == 1);
    REQUIRE(f.sequence.victory().state().finished());
    f.sequence.fallen(Vec3{0}, f.bosses, f.players);
    REQUIRE(f.players[0].actor.save().progress().relics.shards == 0);
    f.bind();
    REQUIRE_FALSE(f.sequence.victory().state().running());
    REQUIRE_FALSE(f.sequence.victory().state().finished());
    f.sequence.fallen(Vec3{0}, f.bosses, f.players);
    REQUIRE(f.players[0].actor.save().progress().relics.hasShard(LevelRef::orderOf(7)));
}
} // namespace
