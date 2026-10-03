#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/core/Types.h"
#include "engine/io/File.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "fixtures/NativeModelFixture.h"
#include "game/world/FallingScenery.h"

namespace {
using namespace gdl;
using namespace gdl::game;
using Catch::Approx;

/** A level with a rock that falls at the origin, a leaf at x 100, a rock a shot brings down
 * at x 200, a sinking rock at x 300 placed for two players, and a barrel that never moves. */
struct Fixture {
    test::FakeRenderDevice device;
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    FallingScenery scenery;

    explicit Fixture(std::string_view name) {
        const auto directory = test::scratchDirectory(name);
        writeTextFile(directory / "world.json", R"({"objects":[{"name":"ROOT","position":[0,0,0]}],
          "bounds":{"min":[-20,-10,-20],"max":[400,20,20]},
          "itemInfos":[{"type":10,"subtype":40,"radius":15,"height":20,"collisionType":1},
                       {"type":10,"subtype":49,"radius":4,"height":6,"collisionType":1},
                       {"type":10,"subtype":52,"radius":10,"height":10,"collisionType":4},
                       {"type":10,"subtype":53,"radius":15,"height":20,"collisionType":1},
                       {"type":10,"subtype":43,"radius":1,"height":3,"collisionType":1}],
          "itemInstances":[
            {"info":0,"name":"ROCKFALL","position":[0,0,0],"params":[40,0,0,0,0,0,0,0,0,0,0,0]},
            {"info":1,"name":"LEAF","position":[100,0,0],"params":[49,0,0,0,0,0,0,0,0,0,0,0]},
            {"info":2,"name":"SHOOTFALL","position":[200,0,0],"params":[52,0,0,0,0,0,0,0,0,0,0,0]},
            {"info":3,"name":"ROCKSINK","position":[300,0,0],"minPlayers":2},
            {"info":4,"name":"BARREL","position":[350,0,0]}]})");
        writeTextFile(directory / "objects.json", R"({"objects":[
          {"index":0,"name":"ROCKFALL","file":"rock.obj","meshTriangles":1},
          {"index":1,"name":"LEAF","file":"rock.obj","meshTriangles":1},
          {"index":2,"name":"SHOOTFALL","file":"rock.obj","meshTriangles":1},
          {"index":3,"name":"ROCKSINK","file":"rock.obj","meshTriangles":1},
          {"index":4,"name":"BARREL","file":"rock.obj","meshTriangles":1}]})");
        writeTextFile(directory / "rock.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl tex0\nf 1 2 3\n");
        writeFile(directory / "skin.png", test::kTinyPng);
        writeTextFile(directory / "textures.json", R"({"bitmaps":[
          {"index":0,"name":"SKIN","file":"skin.png","width":2,"height":2}]})");
        test::convertModelFixture(directory);
        REQUIRE(layout.load(directory));
        REQUIRE(models.load(directory));
        REQUIRE(textures.load(directory));
    }
};

TEST_CASE("falling scenery binds the four subtypes and names the realm's sounds", "[falling]") {
    Fixture f("falling-scenery-bind");
    f.scenery.bind(f.device, f.layout, f.models, f.textures, "F1");
    REQUIRE(f.scenery.size() == 4);
    REQUIRE(f.scenery.piece(0).subtype == FallingScenery::kFallAway);
    REQUIRE(f.scenery.piece(1).subtype == FallingScenery::kLeafFall);
    REQUIRE(f.scenery.piece(2).subtype == FallingScenery::kShootFall);
    REQUIRE(f.scenery.piece(3).subtype == FallingScenery::kRockSink);
    REQUIRE(f.scenery.piece(0).radius == 15.0f);
    REQUIRE(f.scenery.piece(0).height == 20.0f);
    REQUIRE(f.scenery.piece(3).minPlayers == 2);
    REQUIRE_FALSE(f.scenery.piece(3).shown);
    f.scenery.setPlayerCount(2);
    REQUIRE(f.scenery.piece(3).shown);

    REQUIRE(FallingScenery::breakSoundOf("F1") == "S_ROCKBREAKF2");
    REQUIRE(FallingScenery::breakSoundOf("F2") == "S_ROCKBREAKF");
    REQUIRE(FallingScenery::breakSoundOf("I4") == "S_ICEBREAKY");
    REQUIRE(FallingScenery::breakSoundOf("I1") == "S_ICEBREAK");
    REQUIRE(FallingScenery::breakSoundOf("A1") == "S_FALLAWAY");
    REQUIRE(FallingScenery::breakSoundOf("B3") == "S_ROCKBREAK");
    REQUIRE(FallingScenery::breakSoundOf("C1") == "S_LIMBBREAKC");
    REQUIRE(FallingScenery::breakSoundOf("D2") == "S_LIMBBREAK");
    REQUIRE(FallingScenery::breakSoundOf("E1") == "S_ROCKBREAKE");
    REQUIRE(FallingScenery::breakSoundOf("G1") == "S_ROCKBREAKG");
    REQUIRE(FallingScenery::breakSoundOf("H1") == "S_LIMBBREAKH");
    REQUIRE(FallingScenery::breakSoundOf("J1").empty());
    REQUIRE(FallingScenery::breakSoundOf("K1").empty());
    REQUIRE(FallingScenery::breakSoundOf("L1").empty());
    REQUIRE(FallingScenery::breakSoundOf("").empty());
    REQUIRE(FallingScenery::leafSoundOf("D2") == "S_LEAFBREAK");
    REQUIRE(FallingScenery::leafSoundOf("I2") == "S_WOODBREAKI");
    REQUIRE(FallingScenery::leafSoundOf("F1").empty());
}

TEST_CASE("a touch starts a piece once, with its cue, and a shot only the shootable ones",
          "[falling]") {
    Fixture f("falling-scenery-touch");
    f.scenery.bind(f.device, f.layout, f.models, f.textures, "D2");
    // Nothing within reach of a body standing clear of every cylinder.
    REQUIRE(f.scenery.touch(Vec3{50, 0, 0}, 1).empty());
    // Within the record's radius grown by the body's, and its height grown the same way.
    REQUIRE(f.scenery.touch(Vec3{16.5f, 0, 0}, 1).empty());
    REQUIRE(f.scenery.touch(Vec3{15.5f, 21.5f, 0}, 1).empty());
    auto cues = f.scenery.touch(Vec3{15.5f, 20.9f, 0}, 1.0f);
    REQUIRE(cues.size() == 1);
    REQUIRE(cues[0].position == Vec3{0, 0, 0});
    REQUIRE(cues[0].sound == "S_LIMBBREAK");
    REQUIRE(f.scenery.piece(0).started);
    // Once: the piece is already going.
    REQUIRE(f.scenery.touch(Vec3{0, 0, 0}, 1).empty());
    // A leaf gives the leaf sound; the shootable one ignores a touch.
    cues = f.scenery.touch(Vec3{100, 0, 3}, 1);
    REQUIRE(cues.size() == 1);
    REQUIRE(cues[0].sound == "S_LEAFBREAK");
    REQUIRE(f.scenery.touch(Vec3{200, 0, 0}, 1).empty());
    REQUIRE_FALSE(f.scenery.piece(2).started);
    // A shot starts only it, not the sinking rock beside it, and only once.
    REQUIRE(f.scenery.shoot(Vec3{300, 0, 0}, 1).empty());
    cues = f.scenery.shoot(Vec3{209, 0, 0}, 1);
    REQUIRE(cues.size() == 1);
    REQUIRE(cues[0].sound == "S_LIMBBREAK");
    REQUIRE(f.scenery.shoot(Vec3{200, 0, 0}, 1).empty());
    // A piece placed for a larger party is not there to touch.
    REQUIRE(f.scenery.touch(Vec3{300, 0, 0}, 1).empty());
    f.scenery.setPlayerCount(2);
    REQUIRE(f.scenery.touch(Vec3{300, 0, 0}, 1).size() == 1);
}

TEST_CASE("started pieces fall by their subtype's rates and retire under the bottom", "[falling]") {
    Fixture f("falling-scenery-fall");
    f.scenery.bind(f.device, f.layout, f.models, f.textures, "F1");
    f.scenery.setPlayerCount(2);
    f.scenery.update(1);
    for (usize i = 0; i < f.scenery.size(); ++i) {
        REQUIRE(f.scenery.piece(i).motion.position.y == 0.0f);
    }
    REQUIRE(f.scenery.touch(Vec3{0, 0, 0}, 1).size() == 1);
    REQUIRE(f.scenery.touch(Vec3{100, 0, 0}, 1).size() == 1);
    REQUIRE(f.scenery.touch(Vec3{300, 0, 0}, 1).size() == 1);
    REQUIRE(f.scenery.shoot(Vec3{200, 0, 0}, 1).size() == 1);
    constexpr f32 kSlack = 0.0005f; ///< keeps a run of frames from falling a frame short
    f.scenery.update(1.0f / 30 + kSlack);
    const FallingScenery::Piece& rock = f.scenery.piece(0);
    const FallingScenery::Piece& leaf = f.scenery.piece(1);
    const FallingScenery::Piece& shot = f.scenery.piece(2);
    const FallingScenery::Piece& sink = f.scenery.piece(3);
    REQUIRE(rock.motion.velocity.y == -2.0f);
    REQUIRE(rock.motion.position.y == Approx(-2.0f / 30));
    REQUIRE(rock.motion.position.x == 0.0f);
    REQUIRE(rock.motion.rotation.x == Approx(-4 * 0.34906585f / 30)); // instance 0
    REQUIRE(rock.motion.rotation.z == Approx(4 * 0.34906585f / 30));
    REQUIRE(leaf.motion.velocity.y == -1.0f);
    REQUIRE(leaf.motion.rotation.x == Approx(-3 * 0.1745f / 30)); // instance 1
    REQUIRE(shot.motion.velocity.y == -2.0f);
    REQUIRE(shot.motion.rotation.x == Approx(-2 * 0.34906585f / 30)); // instance 2
    REQUIRE(sink.motion.velocity.y == -2.0f);
    REQUIRE(sink.motion.rotation.x == Approx(-1 * 0.01745f / 30)); // instance 3
    f.scenery.draw(f.device, Mat4{1}, {});
    const usize allDrawn = f.device.draws.size();
    REQUIRE(allDrawn >= 4);
    // The bottom is 200 under the floor sentinel 4.5 under the world's lowest point: the
    // rocks reach -214.5 on their 80th frame (2 x 80 x 81 / 2 / 30 = 216), the leaf on
    // its 113th.
    f.scenery.update(78.0f / 30 + kSlack);
    REQUIRE(rock.motion.visible);
    REQUIRE(rock.motion.position.y == Approx(-2.0f * 79 * 80 / 2 / 30).epsilon(0.001f));
    f.scenery.update(1.0f / 30);
    REQUIRE_FALSE(rock.motion.visible);
    REQUIRE_FALSE(sink.motion.visible);
    REQUIRE(leaf.motion.visible);
    f.device.draws.clear();
    f.scenery.draw(f.device, Mat4{1}, {});
    REQUIRE_FALSE(f.device.draws.empty());
    REQUIRE(f.device.draws.size() < allDrawn);
    f.scenery.update(2);
    REQUIRE_FALSE(leaf.motion.visible);
    f.scenery.clear();
    REQUIRE(f.scenery.size() == 0);
}

TEST_CASE("falling scenery draws continuous half frames without changing its native position",
          "[falling][presentation]") {
    Fixture f("falling-scenery-presentation");
    f.scenery.bind(f.device, f.layout, f.models, f.textures, "F1");
    REQUIRE(f.scenery.touch(Vec3{0}, 1).size() == 1);
    f.scenery.update(1.0f / 60);
    f.scenery.update(1.0f / 60);
    const Vec3 native = f.scenery.piece(0).motion.position;
    REQUIRE(native.y < 0);
    const auto at = [&](f32 alpha) {
        f.device.draws.clear();
        f.scenery.draw(f.device, Mat4{1}, {}, alpha);
        REQUIRE_FALSE(f.device.draws.empty());
        return f.device.draws[0].vertices[0].position;
    };
    CHECK(at(0).y == 0);
    CHECK(at(0.5f).y == Approx(native.y * 0.25f));
    CHECK(at(1).y == Approx(native.y * 0.5f));
    const Vec3 boundary = at(1);
    f.scenery.update(1.0f / 60);
    CHECK(at(0) == boundary);
    CHECK(at(1).y == Approx(native.y));
    CHECK(f.scenery.piece(0).motion.position == native);
    CHECK(at(-1).y == Approx(native.y));
}

TEST_CASE("the forest's first level binds its ninety-two falling pieces", "[falling][assets]") {
    const auto directory = test::assetOrSkip("LEVELS/LEVELF1/WORLDS.PS2").parent_path();
    test::assetOrSkip("LEVELS/LEVELF1/objects.ngc");
    WorldLayout layout;
    ModelSet models;
    TextureSet textures;
    test::FakeRenderDevice device;
    REQUIRE(layout.load(directory));
    REQUIRE(models.load(directory));
    REQUIRE(textures.load(directory));
    FallingScenery scenery;
    scenery.bind(device, layout, models, textures, "F1");
    REQUIRE(scenery.size() == 92);
    usize falling = 0;
    usize shootable = 0;
    usize sinking = 0;
    for (usize i = 0; i < scenery.size(); ++i) {
        const FallingScenery::Piece& piece = scenery.piece(i);
        falling += piece.subtype == FallingScenery::kFallAway ? 1 : 0;
        shootable += piece.subtype == FallingScenery::kShootFall ? 1 : 0;
        sinking += piece.subtype == FallingScenery::kRockSink ? 1 : 0;
        REQUIRE(piece.shown);
        REQUIRE_FALSE(piece.started);
    }
    REQUIRE(falling == 46);
    REQUIRE(shootable == 27);
    REQUIRE(sinking == 19);
    scenery.draw(device, Mat4{1}, {});
    REQUIRE(device.draws.size() >= 92);
    // F1ROCKSINK#00 stands at (-22.1875, 17.5, 108.95); a body beside it brings it down, to
    // the level's own sound.
    const auto cues = scenery.touch(Vec3{-22.1875f, 17.5f, 108.953125f}, 1);
    REQUIRE_FALSE(cues.empty());
    for (const FallingCue& cue : cues) {
        REQUIRE(cue.sound == "S_ROCKBREAKF2");
    }
    REQUIRE(scenery.piece(0).started);
    REQUIRE_FALSE(scenery.shoot(Vec3{44.6796875f, 23.46875f, -11.6953125f}, 1).empty());
}
} // namespace
