#include <bit>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "expansions/dreamcast/audio/LegendsDreamcastSounds.h"
#include "expansions/dreamcast/formats/LegendsDreamcastScene.h"
#include "expansions/dreamcast/formats/LegendsSceneMotion.h"

namespace {
using namespace gdl;
using namespace gdl::formats;

void put16(std::vector<u8>& bytes, usize at, u16 value) {
    bytes.at(at) = static_cast<u8>(value);
    bytes.at(at + 1) = static_cast<u8>(value >> 8U);
}
void put32(std::vector<u8>& bytes, usize at, u32 value) {
    put16(bytes, at, static_cast<u16>(value));
    put16(bytes, at + 2, static_cast<u16>(value >> 16U));
}
void putFloat(std::vector<u8>& bytes, usize at, f32 value) {
    put32(bytes, at, std::bit_cast<u32>(value));
}
void putName(std::vector<u8>& bytes, usize at, std::string_view value) {
    for (usize i = 0; i < value.size(); ++i) {
        bytes.at(at + i) = static_cast<u8>(value[i]);
    }
}

struct Fixture {
    static constexpr usize kInfo = 120;
    static constexpr usize kRandom = 200;
    static constexpr usize kInstance = 280;
    std::vector<u8> world = std::vector<u8>(340);
    Fixture() {
        put32(world, 4, 120);
        put32(world, 72, 2);
        put32(world, 76, kInfo);
        put32(world, 80, 1);
        put32(world, 84, kInstance);
        put32(world, 96, 0xf00bab02U);
        put32(world, kInfo, 2);
        put32(world, kInfo + 4, 9);
        put16(world, kInfo + 8, 3);
        put16(world, kInfo + 10, 0x21);
        putFloat(world, kInfo + 12, 2.5f);
        putFloat(world, kInfo + 16, 3.0f);
        putFloat(world, kInfo + 28, -1.0f);
        putName(world, kInfo + 40, "CHEST");
        put32(world, kInfo + 56, 0x01002000);
        put32(world, kInfo + 60, 0x4000);
        put16(world, kInfo + 68, 20);
        put32(world, kRandom, 0xffffffffU);
        put32(world, kRandom + 4, 1);
        put16(world, kRandom + 8, 0);
        world[kInstance + 2] = 3;
        world[kInstance + 3] = 2;
        putName(world, kInstance + 8, "CHEST#02");
        putFloat(world, kInstance + 24, 4.0f);
        putFloat(world, kInstance + 28, 5.0f);
        putFloat(world, kInstance + 32, 6.0f);
        putFloat(world, kInstance + 40, 1.5f);
        for (usize i = 0; i < 12; ++i) {
            world[kInstance + 48 + i] = static_cast<u8>(i + 1);
        }
    }
};

std::vector<u8> realmFixture() {
    // WRLD + three ENMY rows + one LEVL, followed by a 12-byte directory.
    std::vector<u8> bytes(316);
    put32(bytes, 0, 280);
    put32(bytes, 4, 3);
    put32(bytes, 8, 7);
    putName(bytes, 12, "levelG");
    for (usize i = 0; i < 3; ++i) {
        put32(bytes, 64 + i * 24, static_cast<u32>(12 + i));
        put32(bytes, 68 + i * 24, static_cast<u32>(1 + i));
    }
    putName(bytes, 144, "G1");
    putFloat(bytes, 136 + 132, 1);
    for (usize i = 0; i < 6; ++i) {
        put16(bytes, 136 + 60 + i * 2, i < 3 ? static_cast<u16>(i) : 0xffff);
    }
    put32(bytes, 280, 0x57524c44);
    put32(bytes, 284, 8);
    put32(bytes, 288, 1);
    put32(bytes, 292, 0x454e4d59);
    put32(bytes, 296, 64);
    put32(bytes, 300, 3);
    put32(bytes, 304, 0x4c45564c);
    put32(bytes, 308, 136);
    put32(bytes, 312, 1);
    return bytes;
}

std::vector<u8> realmAudioFixture() {
    auto bytes = realmFixture();
    // Add two AUDS rows and three SNDS rows before the existing directory.
    // Use nonzero references so a first-row/incorrect-stride shortcut fails.
    bytes.insert(bytes.begin() + 280, 2 * 60 + 3 * 24, 0);
    bytes.resize(bytes.size() + 24);
    put32(bytes, 0, 472);
    put32(bytes, 4, 5);
    put32(bytes, 508, 0x41554453);
    put32(bytes, 512, 280);
    put32(bytes, 516, 2);
    put32(bytes, 520, 0x534e4453);
    put32(bytes, 524, 400);
    put32(bytes, 528, 3);
    put16(bytes, 136 + 74, 1);
    put16(bytes, 280 + 16, 0xffff); // This fixture isolates wall audio, not entry narration.
    put16(bytes, 340 + 16, 0xffff);
    put16(bytes, 280 + 18, 0);
    put16(bytes, 340 + 18, 2);
    putName(bytes, 400, "WRONG_WALL");
    putName(bytes, 424, "ALSO_WRONG");
    putName(bytes, 448, "SELECTED_WALL");
    return bytes;
}

std::filesystem::path nativeOrSkip() {
    const auto path = test::dataDirectory().parent_path() / "assets/dreamcast/extracted/native";
    if (!std::filesystem::is_directory(path)) {
        SKIP("Dreamcast native assets are not installed");
    }
    return path;
}
} // namespace

TEST_CASE("Dreamcast generator difficulty is read from the selected native level",
          "[legends][dreamcast-generator-cadence][formats]") {
    auto bytes = realmFixture();
    putFloat(bytes, 136 + 132, 1.5f);
    const auto scope = dreamcastLegendsFixtureScope(bytes, "levelg1");
    REQUIRE(scope);
    CHECK(scope->generatorDifficulty == 1.5f);
    for (const f32 invalid : {0.f, -1.f, std::numeric_limits<f32>::infinity(),
                              std::numeric_limits<f32>::quiet_NaN()}) {
        putFloat(bytes, 136 + 132, invalid);
        CHECK_THROWS_AS(dreamcastLegendsFixtureScope(bytes, "levelg1"), FormatError);
    }
}

TEST_CASE("Dreamcast BCAM references are independent of ordinary camera settings",
          "[legends][dreamcast-camera][formats]") {
    auto bytes = realmFixture();
    bytes.insert(bytes.begin() + 280, 168, 0);
    bytes.resize(bytes.size() + 12);
    put32(bytes, 0, 448);
    put32(bytes, 4, 4);
    put32(bytes, 484, 0x4243414d);
    put32(bytes, 488, 280);
    put32(bytes, 492, 2);
    put16(bytes, 136 + 124, 1);
    constexpr usize row = 364;
    put32(bytes, row, BossCameraInfo::kLiveAnchor);
    putFloat(bytes, row + 4, .2f);
    putFloat(bytes, row + 8, .7f);
    putFloat(bytes, row + 12, 64);
    putFloat(bytes, row + 16, 30);
    putFloat(bytes, row + 20, 90);
    putFloat(bytes, row + 24, 30);
    putFloat(bytes, row + 28, .35f);
    putFloat(bytes, row + 32, .23f);
    putFloat(bytes, row + 40, -7);
    putFloat(bytes, row + 52, -12);
    const auto scope = dreamcastLegendsFixtureScope(bytes, "levelg1");
    REQUIRE(scope);
    REQUIRE(scope->bossCamera);
    CHECK_FALSE(scope->camera);
    CHECK(scope->bossCamera->minDistance == 64);
    CHECK(scope->bossCamera->maxDistance == 90);
    CHECK(scope->bossCamera->minAttention == Vec3{0, -7, 0});
    CHECK(scope->bossCamera->maxAttention == Vec3{0, -12, 0});

    SECTION("negative native index leaves the ordinary view alone") {
        put16(bytes, 136 + 124, 0xffff);
        CHECK_FALSE(dreamcastLegendsFixtureScope(bytes, "levelg1")->bossCamera);
    }
    SECTION("missing optional records do not require expansion camera data") {
        CHECK_FALSE(dreamcastLegendsFixtureScope(realmFixture(), "levelg1")->bossCamera);
    }
    SECTION("wrong index cannot read the following section") {
        put16(bytes, 136 + 124, 2);
        CHECK_THROWS_AS(dreamcastLegendsFixtureScope(bytes, "levelg1"), FormatError);
    }
    SECTION("nonfinite camera state is rejected") {
        putFloat(bytes, row + 48, std::numeric_limits<f32>::infinity());
        CHECK_THROWS_AS(dreamcastLegendsFixtureScope(bytes, "levelg1"), FormatError);
    }
    SECTION("native initialization resolves the far-attention and player-range sentinels") {
        putFloat(bytes, row + 24, 0);
        putFloat(bytes, row + 52, 999999);
        const auto camera = dreamcastLegendsFixtureScope(bytes, "levelg1")->bossCamera;
        REQUIRE(camera);
        CHECK(camera->maxPlayerDistance == 90);
        CHECK(camera->maxAttention == camera->minAttention);
    }
}

TEST_CASE("Dreamcast scenes preserve native fixture definitions and placements",
          "[formats][legends][scene]") {
    const Fixture fixture;
    const auto scene = LegendsDreamcastScene::parse(fixture.world);
    REQUIRE(scene.itemInfos.size() == 2);
    const auto& info = scene.itemInfos[0];
    CHECK(info.type == 2);
    CHECK(info.subtype == 9);
    CHECK(info.name == "CHEST");
    CHECK(info.collisionFlags == 0x21);
    CHECK(info.radius == 2.5f);
    CHECK(info.collisionOffset == Vec3(-1, 0, 0));
    CHECK(info.objectFlags == 0x01002000);
    CHECK(info.properties == 0x4000);
    CHECK(info.hitPoints == 20);
    CHECK(scene.itemInfos[1].choices == std::vector<s16>{0});
    REQUIRE(scene.itemInstances.size() == 1);
    const auto& item = scene.itemInstances[0];
    CHECK(item.info == 0);
    CHECK(item.minPlayers == 3);
    CHECK(item.flags == 2);
    CHECK(item.name == "CHEST#02");
    CHECK(item.position == Vec3(4, 5, 6));
    CHECK(item.rotation == Vec3(0, 1.5f, 0));
    CHECK(item.params.front() == 1);
    CHECK(item.params.back() == 12);
}

TEST_CASE("Dreamcast scene reader rejects invalid native references and values",
          "[formats][legends][scene]") {
    Fixture fixture;
    SECTION("header") {
        fixture.world.resize(95);
    }
    SECTION("definition budget") {
        put32(fixture.world, 72, 0xffffffffU);
    }
    SECTION("definition extent") {
        put32(fixture.world, 76, 0xfffffff0U);
    }
    SECTION("random extent") {
        put32(fixture.world, Fixture::kRandom + 4, 37);
    }
    SECTION("negative random count") {
        put32(fixture.world, Fixture::kRandom + 4, 0xffffffffU);
    }
    SECTION("random reference") {
        put16(fixture.world, Fixture::kRandom + 8, 2);
    }
    SECTION("placed reference") {
        put16(fixture.world, Fixture::kInstance, 2);
    }
    SECTION("player count") {
        fixture.world[Fixture::kInstance + 2] = 5;
    }
    SECTION("nonfinite collision") {
        putFloat(fixture.world, Fixture::kInfo + 12, std::numeric_limits<f32>::infinity());
    }
    SECTION("nonfinite position") {
        putFloat(fixture.world, Fixture::kInstance + 28, std::numeric_limits<f32>::quiet_NaN());
    }
    SECTION("negative triangle count") {
        put16(fixture.world, Fixture::kInstance + 6, 0xffff);
    }
    SECTION("collision reference") {
        put16(fixture.world, Fixture::kInstance + 6, 1);
    }
    CHECK_THROWS_AS(LegendsDreamcastScene::parse(fixture.world), FormatError);
}

TEST_CASE("Dreamcast gameplay cameras admit start and game locators but not attract locators",
          "[formats][legends][dreamcast-presentation]") {
    Fixture fixture;
    fixture.world.resize(340 + 4 * 28);
    put32(fixture.world, 88, 4);
    put32(fixture.world, 92, 340);
    for (usize i = 0; i < 4; ++i) {
        const usize at = 340 + i * 28;
        fixture.world[at] = static_cast<u8>(i + 1);
        fixture.world[at + 1] = 7;
        putFloat(fixture.world, at + 4, static_cast<f32>(i));
        putFloat(fixture.world, at + 16, 0.5f);
        putFloat(fixture.world, at + 20, 1);
    }
    const auto scene = LegendsDreamcastScene::parse(fixture.world);
    REQUIRE(scene.cameraMarkers.size() == 2);
    for (usize i = 0; i < 2; ++i) {
        const auto& marker = scene.cameraMarkers[i];
        CHECK(marker.kind == (i == 0 ? LocatorKind::CameraStart : LocatorKind::CameraGame));
        CHECK(marker.delay == 7);
        CHECK(marker.position.x == static_cast<f32>(i));
        CHECK(marker.rotation.x == 0.5f);
        CHECK(marker.rotation.y == 1);
    }
    putFloat(fixture.world, 340 + 20, std::numeric_limits<f32>::infinity());
    CHECK_THROWS_AS(LegendsDreamcastScene::parse(fixture.world), FormatError);
}

TEST_CASE("Dreamcast camera metadata respects disabled bounds and validates its ranges",
          "[formats][legends][dreamcast-presentation]") {
    auto bytes = realmFixture();
    // Insert CAMS before the existing directory, preserving the realm records.
    bytes.insert(bytes.begin() + 280, 108, 0);
    bytes.resize(bytes.size() + 12);
    put32(bytes, 0, 388);
    put32(bytes, 4, 4);
    put32(bytes, 424, 0x43414d53);
    put32(bytes, 428, 280);
    put32(bytes, 432, 1);
    putFloat(bytes, 288, 0.5f);
    putFloat(bytes, 292, -40);
    putFloat(bytes, 304, 40);
    putFloat(bytes, 324, 20);
    putFloat(bytes, 328, 26);
    const auto config = [&] { return dreamcastLegendsFixtureScope(bytes, "g1"); };
    REQUIRE(config()->camera);
    CHECK(config()->camera->boundsMin == Vec3{0});
    CHECK(config()->camera->boundsMax == Vec3{0});
    CHECK_FALSE(config()->camera->authoredBounds);
    bytes[316] = 1;
    CHECK(config()->camera->authoredBounds);
    CHECK(config()->camera->boundsMin.x == -40);
    CHECK(config()->camera->boundsMax.x == 40);
    SECTION("bad reference") {
        put16(bytes, 136 + 72, 1);
    }
    SECTION("inverted radii") {
        putFloat(bytes, 324, 27);
    }
    SECTION("nonfinite radius") {
        putFloat(bytes, 328, std::numeric_limits<f32>::quiet_NaN());
    }
    CHECK_THROWS_AS(config(), FormatError);
}

TEST_CASE("Dreamcast scene reader retains unknown fixture types without inventing behavior",
          "[formats][legends][scene]") {
    Fixture fixture;
    put32(fixture.world, Fixture::kInfo, 0x1234);
    const auto scene = LegendsDreamcastScene::parse(fixture.world);
    CHECK(scene.itemInfos[0].type == 0x1234);
    CHECK(scene.itemInstances.size() == 1);
}

TEST_CASE("Dreamcast wall impact sound follows the selected native audio and sound rows",
          "[formats][legends][scene][dreamcast-sounds]") {
    auto bytes = realmAudioFixture();
    const auto scope = dreamcastLegendsFixtureScope(bytes, "g1");
    REQUIRE(scope);
    CHECK(scope->wallHitSound == "SELECTED_WALL");
    put16(bytes, 136 + 74, 0);
    CHECK(dreamcastLegendsFixtureScope(bytes, "g1")->wallHitSound == "WRONG_WALL");
    put16(bytes, 280 + 18, 1);
    CHECK(dreamcastLegendsFixtureScope(bytes, "g1")->wallHitSound == "ALSO_WRONG");
}

TEST_CASE("Dreamcast entry metadata resolves its selected prefix and rejects invalid references",
          "[formats][legends][scene][dreamcast-speech]") {
    auto bytes = realmAudioFixture();
    put32(bytes, 136 + 4, 3); // Enabled native stage.
    put16(bytes, 340 + 16, 1);
    const auto scope = dreamcastLegendsFixtureScope(bytes, "g1");
    REQUIRE(scope);
    CHECK(scope->entrySound == "ALSO_WRONG"); // The selected SNDS1, not first/last row.
    CHECK(scope->stageNameSound == "S_G1NAME");
    put16(bytes, 340 + 16, 3);
    CHECK_THROWS_AS(dreamcastLegendsFixtureScope(bytes, "g1"), FormatError);
}

TEST_CASE("Dreamcast missing or disabled optional wall sounds do not invent a cue",
          "[formats][legends][scene][dreamcast-sounds]") {
    auto bytes = realmAudioFixture();
    SECTION("no audio sections") {
        bytes = realmFixture();
    }
    SECTION("missing AUDS") {
        put32(bytes, 508, 0x12345678);
    }
    SECTION("empty AUDS") {
        put32(bytes, 516, 0);
    }
    SECTION("level audio disabled") {
        put16(bytes, 136 + 74, 0xffff);
    }
    SECTION("wall cue disabled") {
        put16(bytes, 340 + 18, 0xffff);
    }
    SECTION("disabled cue does not require SNDS") {
        put16(bytes, 340 + 18, 0xffff);
        put32(bytes, 520, 0x12345678);
    }
    const auto scope = dreamcastLegendsFixtureScope(bytes, "g1");
    REQUIRE(scope);
    CHECK(scope->wallHitSound.empty());
}

TEST_CASE("Dreamcast wall sound metadata rejects invalid references and extents",
          "[formats][legends][scene][dreamcast-sounds]") {
    auto bytes = realmAudioFixture();
    SECTION("level audio index") {
        put16(bytes, 136 + 74, 2);
    }
    SECTION("wall sound index") {
        put16(bytes, 340 + 18, 3);
    }
    SECTION("referenced SNDS absent") {
        put32(bytes, 520, 0x12345678);
    }
    SECTION("referenced SNDS empty") {
        put32(bytes, 528, 0);
    }
    SECTION("truncated audio rows") {
        put32(bytes, 516, 3);
    }
    SECTION("truncated sound rows") {
        put32(bytes, 528, 4);
    }
    CHECK_THROWS_AS(dreamcastLegendsFixtureScope(bytes, "g1"), FormatError);
}

TEST_CASE("Dreamcast B1 wall cue resolves in the native audio directory",
          "[formats][legends][scene][dreamcast-sounds][assets]") {
    const auto root = nativeOrSkip();
    if (!std::filesystem::is_regular_file(root.parent_path() / "disc/AUDIO/AUD_DATA.ROM"))
        SKIP("Optional Dreamcast audio is not installed");
    const auto scope = dreamcastLegendsFixtureScope(readFile(root / "worlddata/mount.wad"), "b1");
    REQUIRE(scope);
    // B1 selects AUDS0, whose +18 value is SNDS2. The native fixed-width
    // key is intentionally not expanded to a guessed full-length spelling.
    CHECK(scope->wallHitSound == "S_WEAPONHITSTON");
    game::LegendsDreamcastSounds sounds;
    REQUIRE(sounds.open(root));
    CHECK(sounds.namedToken(scope->wallHitSound));
}

TEST_CASE("Dreamcast fixture libraries and generator species follow selected native roster",
          "[formats][legends][scene]") {
    const auto bytes = realmFixture();
    const auto scope = dreamcastLegendsFixtureScope(bytes, "levelG1");
    REQUIRE(scope);
    CHECK(scope->realmType == 7);
    CHECK(scope->archives == std::vector<std::string>{"levels/levelg1", "items/levelg1",
                                                      "items/levelg", "generators", "powerups",
                                                      "weapons"});
    CHECK(scope->enemyClasses[1] == 12);
    CHECK(scope->enemyClasses[2] == 13);
    CHECK(scope->enemyClasses[3] == 14);
    CHECK_FALSE(dreamcastLegendsFixtureScope(bytes, "levela1"));
    CHECK(dreamcastLegendsWorldDataFile("levelG1") == "worlddata/town.wad");
    CHECK(dreamcastLegendsWorldDataFile("t1") == "worlddata/tower.wad");
    CHECK(dreamcastLegendsWorldDataFile("../../g1").empty());
    ItemInfoRecord info;
    info.type = 3;
    info.name = "GRU";
    ItemInstanceRecord item;
    item.name = "IGNORED";
    item.params[0] = 2;
    CHECK(dreamcastLegendsFixtureName(info, item, *scope) == "GEN_ZOM2");
    info.name = "RAT";
    CHECK(dreamcastLegendsFixtureName(info, item, *scope) == "GEN_MAG2");
    info.name = "KNI";
    CHECK(dreamcastLegendsFixtureName(info, item, *scope) == "GEN_ZOM2");
    info.name = "TRO";
    item.params[0] = 0;
    CHECK(dreamcastLegendsFixtureName(info, item, *scope) == "GEN_TRO1");
    auto special = *scope;
    special.realmType = 5;
    CHECK(dreamcastLegendsFixtureName(info, item, special) == "GEN_SPECIAL2");
    special.realmType = 6;
    CHECK(dreamcastLegendsFixtureName(info, item, special) == "GEN_SPECIAL3");
    info.type = 5;
    info.name = "BRIDGEPAD";
    item.name = "TRIGGER2";
    CHECK(dreamcastLegendsFixtureName(info, item, *scope) == "TRIGGER2");
    item.name.clear();
    CHECK(dreamcastLegendsFixtureName(info, item, *scope) == "BRIDGEPAD");
    item.flags = 2;
    CHECK(dreamcastLegendsFixtureName(info, item, *scope).empty());
    item.flags = 0;
    info.type = 10;
    info.subtype = 41;
    info.name = "SAFEROCK";
    item.name = "IGNORED";
    item.params = {};
    item.params[2] = 3;
    CHECK(dreamcastLegendsFixtureName(info, item, *scope) == "SAFEROCK3");
    info.subtype = 42;
    item.params[0] = 41;
    CHECK(dreamcastLegendsFixtureName(info, item, *scope) == "SAFEROCK3");
    item.params[0] = 42;
    CHECK(dreamcastLegendsFixtureName(info, item, *scope) == "IGNORED");
}

TEST_CASE("Dreamcast fixture scopes reject invalid worlddata references",
          "[formats][legends][scene]") {
    auto bytes = realmFixture();
    SECTION("directory") {
        put32(bytes, 0, 0xfffffff0U);
    }
    SECTION("section extent") {
        put32(bytes, 300, 4);
    }
    SECTION("duplicate section") {
        put32(bytes, 304, 0x454e4d59);
    }
    SECTION("missing realm") {
        put32(bytes, 288, 0);
    }
    SECTION("roster reference") {
        put16(bytes, 196, 3);
    }
    SECTION("negative roster reference") {
        put16(bytes, 196, 0xfffe);
    }
    SECTION("unsafe library name") {
        bytes[12] = '/';
    }
    CHECK_THROWS_AS(dreamcastLegendsFixtureScope(bytes, "g1"), FormatError);
}

TEST_CASE("Dreamcast scene fixtures decode every native level",
          "[formats][legends][scene][assets]") {
    const auto root = nativeOrSkip();
    usize levels = 0;
    usize definitions = 0;
    usize placements = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root / "levels")) {
        if (!entry.is_directory() || !entry.path().filename().string().starts_with("level")) {
            continue;
        }
        CAPTURE(entry.path().string());
        const auto bytes = readFile(entry.path() / "worlds.rom");
        const auto scene = LegendsDreamcastScene::parse(bytes);
        CHECK(scene.itemInfos.size() == readU32LE(bytes, 72));
        CHECK(scene.itemInstances.size() == readU32LE(bytes, 80));
        const auto metadata =
            root / dreamcastLegendsWorldDataFile(entry.path().filename().string());
        const auto scope =
            dreamcastLegendsFixtureScope(readFile(metadata), entry.path().filename().string());
        REQUIRE(scope.has_value() == (entry.path().filename() != "levela2"));
        if (scope) {
            for (const auto& item : scene.itemInstances) {
                const auto& info = scene.itemInfos[static_cast<usize>(item.info)];
                if (info.type == 3 && (item.flags & 2U) == 0) {
                    CHECK(dreamcastLegendsFixtureName(info, item, *scope).starts_with("GEN_"));
                }
            }
        }
        definitions += scene.itemInfos.size();
        placements += scene.itemInstances.size();
        ++levels;
    }
    CHECK(levels == 41);
    CHECK(definitions == 2947);
    CHECK(placements == 9655);
}

TEST_CASE("Dreamcast fixture model libraries preserve every native model slot",
          "[formats][legends][scene][assets]") {
    const auto root = nativeOrSkip();
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(root / "items")) {
        if (entry.is_directory() && entry.path().filename().string().starts_with("level") &&
            std::filesystem::is_regular_file(entry.path() / "objects.rom")) {
            paths.push_back(entry.path());
        }
    }
    for (const auto* name : {"generators", "powerups", "weapons"}) {
        paths.push_back(root / name);
    }
    usize vertices = 0;
    for (const auto& path : paths) {
        CAPTURE(path.string());
        const auto bytes = readFile(path / "objects.rom");
        const auto models = decodeDreamcastModelLibrary(bytes);
        CHECK(models.size() == readU32LE(bytes, 96));
        for (const auto& model : models) {
            for (const auto& vertex : model.mesh.vertices) {
                CHECK(std::isfinite(vertex.position.x));
                CHECK(std::isfinite(vertex.position.y));
                CHECK(std::isfinite(vertex.position.z));
            }
            vertices += model.mesh.vertices.size();
        }
    }
    CHECK(paths.size() == 21);
    CHECK(vertices > 10000);
}

TEST_CASE("Dreamcast visible fixtures resolve within their native level and item libraries",
          "[formats][legends][scene-bindings][assets]") {
    const auto root = nativeOrSkip();
    std::map<std::string, std::set<std::string>> cachedNames;
    const auto names = [&](const std::string& relative) -> const std::set<std::string>& {
        const auto [found, inserted] = cachedNames.try_emplace(relative);
        if (!inserted) {
            return found->second;
        }
        const auto directory = root / relative;
        if (std::filesystem::is_regular_file(directory / "objects.rom")) {
            for (const auto& model :
                 decodeDreamcastModelLibrary(readFile(directory / "objects.rom"))) {
                found->second.insert(model.name);
            }
        }
        if (std::filesystem::is_regular_file(directory / "anim.rom")) {
            for (const auto& tree :
                 parseDreamcastAnimation(readFile(directory / "anim.rom")).trees) {
                found->second.insert(tree.name);
            }
        }
        return found->second;
    };
    usize bound = 0;
    std::vector<std::string> missing;
    for (const auto& entry : std::filesystem::directory_iterator(root / "levels")) {
        if (!entry.is_directory() || !entry.path().filename().string().starts_with("level")) {
            continue;
        }
        const auto level = entry.path().filename().string();
        const auto scope = dreamcastLegendsFixtureScope(
            readFile(root / dreamcastLegendsWorldDataFile(level)), level);
        if (!scope) {
            continue;
        }
        const auto scene = LegendsDreamcastScene::parse(readFile(entry.path() / "worlds.rom"));
        for (const auto& item : scene.itemInstances) {
            if (item.minPlayers > 1) {
                continue;
            }
            const auto& info = scene.itemInfos[static_cast<usize>(item.info)];
            if (info.type != 1 && info.type != 2 && info.type != 3 && info.type != 5 &&
                info.type != 7 && info.type != 8 && info.type != 9 && info.type != 10 &&
                info.type != 11) {
                continue;
            }
            const auto name = dreamcastLegendsFixtureName(info, item, *scope);
            if (name.empty()) {
                continue;
            }
            bool resolved = false;
            for (const auto& relative : scope->archives) {
                const auto& library = names(relative);
                for (const auto* suffix : {"", "L1", "L1ROOT"}) {
                    resolved |= library.contains(name + suffix);
                }
            }
            if (resolved) {
                ++bound;
            } else {
                missing.emplace_back(level).append(":").append(name);
            }
        }
    }
    CHECK(bound == 3982);
    // Retain the actual shipped gap; do not borrow an unrelated level's wall.
    CHECK(missing == std::vector<std::string>{"levelh3:H3SHOOTWALL_UP"});
}
