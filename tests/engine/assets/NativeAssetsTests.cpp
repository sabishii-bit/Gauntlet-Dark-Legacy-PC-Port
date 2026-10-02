#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine/assets/AnimationSet.h"
#include "engine/assets/BitmapFont.h"
#include "engine/assets/ItemArchive.h"
#include "engine/assets/MessageTable.h"
#include "engine/assets/ModelSet.h"
#include "engine/assets/SoundSet.h"
#include "engine/assets/TextureSet.h"
#include "engine/assets/WorldData.h"
#include "engine/assets/WorldLayout.h"
#include "engine/core/Strings.h"
#include "engine/core/Types.h"
#include "engine/io/ByteReader.h"
#include "engine/io/File.h"
#include "engine/world/WorldCollision.h"

#include "FakeRenderDevice.h"
#include "TestSupport.h"
#include "formats/AnimationTree.h"
#include "formats/GeometryStream.h"
#include "formats/ModelArchive.h"
#include "formats/WorldDataWad.h"

namespace {
using namespace gdl;
using test::ByteWriter;

/** A freshly generated comparison export can be supplied without touching installed assets. */
std::filesystem::path referenceExport(std::string_view relative) {
    if (const char* root = std::getenv("GDL_NATIVE_REFERENCE_DIR")) {
        const auto path = std::filesystem::path(root) / relative;
        REQUIRE(std::filesystem::exists(path));
        return path;
    }
    SKIP("Set GDL_NATIVE_REFERENCE_DIR to a fresh gdlunpack export for comparison; installed "
         "exports may be stale");
    return {};
}

/** One empty mesh and one 4x4 RGB5A3 tile, with an alias for the tile. */
std::vector<u8> tinyArchive(u16 flags = 0) {
    constexpr u32 kObjects = 160;
    constexpr u32 kBitmaps = 224;
    constexpr u32 kObjectDefs = 288;
    constexpr u32 kBitmapDefs = 312;
    ByteWriter bytes;
    bytes.putZeros(64).putU32(formats::ModelArchive::kVersion13);
    bytes.putU32(1).putU32(1).putU32(1).putU32(2);
    bytes.putU32(kObjects).putU32(kBitmaps).putU32(kObjectDefs).putU32(kBitmapDefs);
    bytes.putZeros(kObjects - bytes.size());
    bytes.putZeros(64); // empty mesh: no subobjects
    bytes.putU8(0).putZeros(7).putU16(flags).putU16(0).putU32(0);
    bytes.putZeros(4).putU16(1).putU16(4).putU16(4).putZeros(38);
    REQUIRE(bytes.size() == kObjectDefs);
    bytes.putText("EMPTY").putZeros(11).putU32(0).putU16(0).putU16(0);
    for (const auto* name : {"RED", "ALIAS"}) {
        const std::string text(name);
        bytes.putText(text).putZeros(30 - text.size()).putU16(0).putU16(4).putU16(4);
    }
    return bytes.bytes();
}

std::vector<u8> tinyAnimation() {
    ByteWriter bytes;
    bytes.putU16(1).putU16(8).putU32(24).putZeros(16);
    bytes.putText("EMPTY").putZeros(27).putU32(60);
    bytes.putU32(56).putU32(0).putU32(0).putU32(56).putU32(1).putU32(0);
    bytes.putText("EMPTY").putZeros(25).putU16(0);
    bytes.putText("ROOT").putZeros(28).putZeros(12);
    bytes.putU16(0).putU16(0).putU32(0).putU32(0).putS32(-1);
    return bytes.bytes();
}

TEST_CASE("native archives load without any exported files", "[native-assets]") {
    const auto path = test::scratchDirectory("native-archive");
    writeFile(path / "OBJECTS.NGC", tinyArchive());
    std::vector<u8> pixels;
    for (usize i = 0; i < 16; ++i) {
        pixels.push_back(0xFC);
        pixels.push_back(0x00);
    }
    writeFile(path / "TEXTURES.NGC", pixels);
    writeFile(path / "ANIM.PS2", tinyAnimation());
    ItemArchive archive;
    REQUIRE(archive.load(path.parent_path() / "NATIVE-ARCHIVE"));
    REQUIRE(archive.models.find("empty") == 0);
    REQUIRE(archive.models.mesh(0).vertices.empty());
    REQUIRE(archive.textures.find("red") == 0);
    REQUIRE(archive.textures.find("alias") == 0);
    REQUIRE(archive.textures.image(0).pixel(0, 0) == Color::rgba(255, 0, 0, 255));
    REQUIRE(archive.trees.find("empty") == 0);
    REQUIRE(archive.trees.tree(0).nodes.size() == 1);
    test::FakeRenderDevice device;
    archive.textures.texture(device, 0);
    archive.textures.texture(device, 0);
    REQUIRE(device.texturesCreated == 1);
    archive.clear();
    REQUIRE_FALSE(archive.models.loaded());
    REQUIRE_FALSE(archive.textures.loaded());
    REQUIRE_FALSE(archive.trees.loaded());
    REQUIRE(archive.load(path));
}

TEST_CASE("native invalid-picture slots need no pixels and corrupt native files never fall back",
          "[native-assets]") {
    const auto path = test::scratchDirectory("native-errors");
    writeFile(path / "objects.ngc", tinyArchive(formats::bitmap_flags::kInvalid));
    TextureSet textures;
    REQUIRE(textures.load(path));
    REQUIRE(textures.image(0).pixels == std::vector<u8>(4, 0));
    writeTextFile(path / "textures.json", R"({"bitmaps":[{"name":"STALE","width":1,
        "height":1,"flags":256,"file":"unused"}]})");
    writeFile(path / "objects.ngc", std::array<u8, 1>{0});
    REQUIRE_FALSE(textures.load(path));
    REQUIRE_FALSE(textures.find("RED"));
    REQUIRE_FALSE(textures.find("STALE"));
    REQUIRE(textures.size() == 0);
    writeFile(path / "objects.ngc", tinyArchive());
    REQUIRE_FALSE(textures.load(path)); // local pixels cannot silently become invisible
    ModelSet models;
    REQUIRE(models.load(path));
    writeFile(path / "objects.ngc", std::array<u8, 1>{0});
    REQUIRE_FALSE(models.load(path));
    REQUIRE_FALSE(models.find("EMPTY"));
    AnimationSet animations;
    writeFile(path / "anim.ps2", tinyAnimation());
    REQUIRE(animations.load(path));
    writeFile(path / "anim.ps2", std::array<u8, 1>{0});
    REQUIRE_FALSE(animations.load(path));
    REQUIRE_FALSE(animations.find("EMPTY"));
}

TEST_CASE("native font glyphs are usable without a manifest and failed reload clears lookup",
          "[native-assets]") {
    const auto path = test::scratchDirectory("native-font");
    ByteWriter bytes;
    bytes.putS32(0).putS32(12).putS32(0);
    bytes.putS32(65).putS32(7).putS32(10).putS32(20);
    writeFile(path / "SMALL.FNT", bytes.bytes());
    BitmapFont font;
    REQUIRE(font.load(path / "small.json", 5));
    REQUIRE(font.height() == 12);
    REQUIRE(font.glyph('A') != nullptr);
    REQUIRE(font.glyph('A')->x == 10);
    writeFile(path / "SMALL.FNT", std::array<u8, 1>{0});
    REQUIRE_FALSE(font.load(path / "small.json", 5));
    REQUIRE(font.glyph('A') == nullptr);
}

void compareAnimations(const AnimationSet& native, const AnimationSet& exported) {
    REQUIRE(native.size() == exported.size());
    REQUIRE(native.textureAnimations().size() == exported.textureAnimations().size());
    for (usize i = 0; i < native.textureAnimations().size(); ++i) {
        const auto& a = native.textureAnimations()[i];
        const auto& b = exported.textureAnimations()[i];
        REQUIRE(a.name == b.name);
        REQUIRE(a.frameName == b.frameName);
        REQUIRE(a.texture == b.texture);
        REQUIRE(a.source == b.source);
        REQUIRE(a.frames == b.frames);
        REQUIRE(a.start == b.start);
        REQUIRE(a.rate == b.rate);
        REQUIRE(a.offset == b.offset);
        REQUIRE(a.flag == b.flag);
    }
    REQUIRE(native.particleTemplates().size() == exported.particleTemplates().size());
    for (usize i = 0; i < native.particleTemplates().size(); ++i) {
        const auto& a = native.particleTemplates()[i];
        const auto& b = exported.particleTemplates()[i];
        REQUIRE(a.id == b.id);
        REQUIRE(a.preset == b.preset);
        REQUIRE(a.flags == b.flags);
        REQUIRE(a.flagMask == b.flagMask);
        REQUIRE(a.enables == b.enables);
        REQUIRE(a.maxParticles == b.maxParticles);
        REQUIRE(a.maxDirections == b.maxDirections);
        REQUIRE(a.maxPositions == b.maxPositions);
        REQUIRE(a.emitterLife == b.emitterLife);
        REQUIRE(a.particleLife == b.particleLife);
        REQUIRE(a.angle == b.angle);
        REQUIRE(a.textureCount == b.textureCount);
        REQUIRE(a.texture == b.texture);
        REQUIRE(a.direction == b.direction);
        REQUIRE(a.volume == b.volume);
        REQUIRE(a.rate == b.rate);
        REQUIRE(a.rateRandom == b.rateRandom);
        REQUIRE(a.gravity == b.gravity);
        REQUIRE(a.drag == b.drag);
        REQUIRE(a.speed == b.speed);
        REQUIRE(a.rgba == b.rgba);
        REQUIRE(a.width == b.width);
        REQUIRE(a.delay == b.delay);
    }
    for (u32 i = 0; i < native.size(); ++i) {
        const auto& a = native.tree(i);
        const auto& b = exported.tree(i);
        CAPTURE(a.name);
        REQUIRE(a.name == b.name);
        REQUIRE(a.prefix == b.prefix);
        REQUIRE(a.nodes.size() == b.nodes.size());
        REQUIRE(a.sequences.size() == b.sequences.size());
        for (usize n = 0; n < a.nodes.size(); ++n) {
            const auto& x = a.nodes[n];
            const auto& y = b.nodes[n];
            REQUIRE(x.name == y.name);
            REQUIRE(x.object == y.object);
            REQUIRE(x.type == y.type);
            REQUIRE(x.flags == y.flags);
            REQUIRE(x.objectFlags == y.objectFlags);
            REQUIRE(x.parent == y.parent);
            REQUIRE(x.position == y.position);
            REQUIRE(x.particle == y.particle);
            REQUIRE(x.direction == y.direction);
            REQUIRE(x.textureAnimation == y.textureAnimation);
            REQUIRE(x.objectFrames.size() == y.objectFrames.size());
            for (usize f = 0; f < x.objectFrames.size(); ++f) {
                REQUIRE(x.objectFrames[f].object == y.objectFrames[f].object);
                REQUIRE(x.objectFrames[f].start == y.objectFrames[f].start);
                REQUIRE(x.objectFrames[f].frames == y.objectFrames[f].frames);
            }
        }
        for (usize s = 0; s < a.sequences.size(); ++s) {
            const auto& x = a.sequences[s];
            const auto& y = b.sequences[s];
            REQUIRE(x.name == y.name);
            REQUIRE(x.frames == y.frames);
            REQUIRE(x.frameRate == y.frameRate);
            REQUIRE(x.repeats == y.repeats);
            REQUIRE(x.fixesPosition == y.fixesPosition);
            REQUIRE(x.flags == y.flags);
            REQUIRE(x.textureAnimationStart == y.textureAnimationStart);
            REQUIRE(x.textureAnimationCount == y.textureAnimationCount);
            REQUIRE(x.trackOfNode == y.trackOfNode);
            REQUIRE(x.tracks.size() == y.tracks.size());
            for (usize t = 0; t < x.tracks.size(); ++t) {
                REQUIRE(x.tracks[t].node == y.tracks[t].node);
                REQUIRE(x.tracks[t].flags == y.tracks[t].flags);
                REQUIRE(x.tracks[t].frames == y.tracks[t].frames);
                REQUIRE(x.tracks[t].values.size() == y.tracks[t].values.size());
                for (usize k = 0; k < x.tracks[t].values.size(); ++k) {
                    // The text exporter writes these arrays to six significant digits.
                    REQUIRE(x.tracks[t].values[k] ==
                            Catch::Approx(y.tracks[t].values[k]).epsilon(0.00001).margin(0.000001));
                }
            }
        }
    }
}

TEST_CASE("retail meshes textures and animation metadata load directly",
          "[native-assets][assets][unpacked]") {
    for (const auto* name : {"STATIC", "WEAPONS", "ITEMS/LEVELL", "MONSTERS/YETI",
                             "MONSTERS/WRAITH", "PLAYERS/JES/SFXGRE"}) {
        CAPTURE(name);
        const auto source = test::assetOrSkip(std::string(name) + "/objects.ngc").parent_path();
        const auto exportPath = referenceExport(name);
        ItemArchive native;
        ItemArchive exported;
        REQUIRE(native.load(source));
        REQUIRE(exported.load(exportPath));
        compareAnimations(native.trees, exported.trees);
        REQUIRE(native.textures.size() == exported.textures.size());
        for (u32 i = 0; i < native.textures.size(); ++i) {
            const auto& a = native.textures.entry(i);
            const auto& b = exported.textures.entry(i);
            CAPTURE(i, a.name);
            REQUIRE(a.name == b.name);
            REQUIRE(a.width == b.width);
            REQUIRE(a.height == b.height);
            REQUIRE(a.flags == b.flags);
            REQUIRE(a.frames == b.frames);
            REQUIRE(a.clampU == b.clampU);
            REQUIRE(a.clampV == b.clampV);
            REQUIRE(a.halfResolution == b.halfResolution);
            REQUIRE(a.noPicture == b.noPicture);
            if (!a.external()) {
                REQUIRE(native.textures.image(i).pixels == exported.textures.image(i).pixels);
            }
        }
        const auto archive = formats::ModelArchive::parse(readFile(source / "objects.ngc"));
        REQUIRE(native.models.size() == exported.models.size());
        for (u32 i = 0; i < native.models.size(); ++i) {
            CAPTURE(i);
            REQUIRE(native.models.entry(i).name == exported.models.entry(i).name);
            Mesh decoded;
            for (const auto& sub : archive.objects()[i].subObjects) {
                formats::decodeGeometryStream(sub.geometry, sub.textureIndex, sub.lightmapIndex,
                                              decoded);
            }
            const auto& mesh = native.models.mesh(i);
            REQUIRE(mesh.vertices == decoded.vertices); // no lossy OBJ round trip
            REQUIRE(mesh.parts.size() == decoded.parts.size());
            REQUIRE(mesh.prelit == decoded.prelit);
            for (usize p = 0; p < mesh.parts.size(); ++p) {
                REQUIRE(mesh.parts[p].texture == decoded.parts[p].texture);
                REQUIRE(mesh.parts[p].lightmap == decoded.parts[p].lightmap);
                REQUIRE(mesh.parts[p].indices == decoded.parts[p].indices);
            }
            REQUIRE(mesh.triangleCount() == exported.models.mesh(i).triangleCount());
        }
    }
}

TEST_CASE("native bank calls and decoded samples agree with exported audio",
          "[native-assets][assets][unpacked]") {
    const auto audio = test::assetOrSkip("AUDIO/AUDATPS2.ROM").parent_path();
    for (const auto* name : {"COMMON", "YETI", "TOWAMB"}) {
        CAPTURE(name);
        const auto exportedPath = referenceExport(std::string("audio/") + name);
        SoundSet native;
        SoundSet exported;
        REQUIRE(native.load(audio / name));
        REQUIRE(exported.load(exportedPath));
        REQUIRE(native.size() == exported.size());
        for (u32 i = 0; i < native.size(); ++i) {
            const auto& a = native.entry(i);
            const auto& b = exported.entry(i);
            REQUIRE(a.name == b.name);
            REQUIRE(a.id == b.id);
            REQUIRE(a.volume == b.volume);
            REQUIRE(a.duration == b.duration);
            REQUIRE(a.sequence.size() == b.sequence.size());
            for (usize j = 0; j < a.sequence.size(); ++j) {
                REQUIRE(a.sequence[j].sample == b.sequence[j].sample);
                REQUIRE(a.sequence[j].loopStart == b.sequence[j].loopStart);
                REQUIRE(a.sequence[j].loopBack == b.sequence[j].loopBack);
                const auto& x = native.sample(a.sequence[j].sample);
                const auto& y = exported.sample(b.sequence[j].sample);
                REQUIRE(x.sampleRate == y.sampleRate);
                REQUIRE(x.channels == y.channels);
                REQUIRE(x.samples == y.samples);
            }
        }
    }
}

TEST_CASE("native text preserves pages lists and font selection",
          "[native-assets][assets][unpacked]") {
    for (const auto* name : {"scroll_e", "hints_e"}) {
        const auto source = test::assetOrSkip(std::string("TEXT/") + name + ".rom");
        const auto exportFile = referenceExport(std::string("text/") + name + ".json");
        MessageTable native;
        MessageTable exported;
        REQUIRE(native.load(source));
        REQUIRE(exported.load(exportFile));
        REQUIRE(native.size() == exported.size());
        REQUIRE(native.fonts() == exported.fonts());
        REQUIRE(native.lists().size() == exported.lists().size());
        for (u32 i = 0; i < native.size(); ++i) {
            const auto& a = native.message(i);
            const auto& b = exported.message(i);
            REQUIRE(a.name == b.name);
            REQUIRE(a.font == b.font);
            REQUIRE(a.scale == b.scale);
            REQUIRE(a.shadowScale == b.shadowScale);
            REQUIRE(a.pages == b.pages);
        }
        for (usize i = 0; i < native.lists().size(); ++i) {
            REQUIRE(native.lists()[i].name == exported.lists()[i].name);
            REQUIRE(native.lists()[i].messages == exported.lists()[i].messages);
        }
    }
}
TEST_CASE("native worlds preserve placements trigger payloads collision and animation",
          "[native-assets][assets][unpacked]") {
    for (const auto* tag : {"L1", "J1", "C1"}) {
        CAPTURE(tag);
        const std::string relative = std::string("LEVELS/LEVEL") + tag;
        const auto source = test::assetOrSkip(relative + "/worlds.ps2").parent_path();
        const auto old = referenceExport(relative);
        WorldLayout native;
        WorldLayout exported;
        REQUIRE(native.load(source));
        REQUIRE(exported.load(old));
        REQUIRE(native.minBounds() == exported.minBounds());
        REQUIRE(native.maxBounds() == exported.maxBounds());
        REQUIRE(native.objects().size() == exported.objects().size());
        for (usize i = 0; i < native.objects().size(); ++i) {
            const auto& a = native.objects()[i];
            const auto& b = exported.objects()[i];
            REQUIRE(a.name == b.name);
            REQUIRE(a.position == b.position);
            REQUIRE(a.flags == b.flags);
            REQUIRE(a.objectFlags == b.objectFlags);
            REQUIRE(a.next == b.next);
            REQUIRE(a.child == b.child);
            REQUIRE(a.parent == b.parent);
            REQUIRE(a.radius == b.radius);
            REQUIRE(a.noCollision == b.noCollision);
        }
        REQUIRE(native.locators().size() == exported.locators().size());
        for (usize i = 0; i < native.locators().size(); ++i) {
            const auto& a = native.locators()[i];
            const auto& b = exported.locators()[i];
            REQUIRE(a.kind == b.kind);
            REQUIRE(a.next == b.next);
            REQUIRE(a.delay == b.delay);
            REQUIRE(a.position == b.position);
            REQUIRE(a.rotation == b.rotation);
        }
        REQUIRE(native.animations().size() == exported.animations().size());
        for (usize i = 0; i < native.animations().size(); ++i) {
            const auto& a = native.animations()[i];
            const auto& b = exported.animations()[i];
            REQUIRE(a.object == b.object);
            REQUIRE(a.frames == b.frames);
            REQUIRE(a.state == b.state);
            REQUIRE(a.start == b.start);
            REQUIRE(a.track.flags == b.track.flags);
            REQUIRE(a.track.frames == b.track.frames);
            REQUIRE(a.track.values.size() == b.track.values.size());
            for (usize k = 0; k < a.track.values.size(); ++k) {
                REQUIRE(a.track.values[k] ==
                        Catch::Approx(b.track.values[k]).epsilon(0.00001).margin(0.000001));
            }
        }
        REQUIRE(native.itemInfos().size() == exported.itemInfos().size());
        for (usize i = 0; i < native.itemInfos().size(); ++i) {
            const auto& a = native.itemInfos()[i];
            const auto& b = exported.itemInfos()[i];
            REQUIRE(a.type == b.type);
            REQUIRE(a.subtype == b.subtype);
            REQUIRE(a.name == b.name);
            REQUIRE(a.radius == b.radius);
            REQUIRE(a.height == b.height);
            REQUIRE(a.xSize == b.xSize);
            REQUIRE(a.zSize == b.zSize);
            REQUIRE(a.collisionType == b.collisionType);
            REQUIRE(a.collisionFlags == b.collisionFlags);
            REQUIRE(a.collisionOffset == b.collisionOffset);
            REQUIRE(a.objectFlags == b.objectFlags);
            REQUIRE(a.properties == b.properties);
            REQUIRE(a.value == b.value);
            REQUIRE(a.armor == b.armor);
            REQUIRE(a.hitPoints == b.hitPoints);
            REQUIRE(a.activeType == b.activeType);
            REQUIRE(a.activeOff == b.activeOff);
            REQUIRE(a.activeOn == b.activeOn);
            REQUIRE(a.choices == b.choices);
        }
        REQUIRE(native.itemInstances().size() == exported.itemInstances().size());
        for (usize i = 0; i < native.itemInstances().size(); ++i) {
            const auto& a = native.itemInstances()[i];
            const auto& b = exported.itemInstances()[i];
            REQUIRE(a.info == b.info);
            REQUIRE(a.flags == b.flags);
            REQUIRE(a.minPlayers == b.minPlayers);
            REQUIRE(a.name == b.name);
            REQUIRE(a.position == b.position);
            REQUIRE(a.rotation == b.rotation);
            REQUIRE(a.params == b.params);
            REQUIRE(a.collision.size() == b.collision.size());
            for (usize j = 0; j < a.collision.size(); ++j) {
                REQUIRE(a.collision[j].normal == b.collision[j].normal);
                REQUIRE(a.collision[j].vertices == b.collision[j].vertices);
            }
        }
        WorldCollision collision;
        WorldCollision oldCollision;
        REQUIRE(collision.load(source, native));
        REQUIRE(oldCollision.load(old, exported));
        REQUIRE(collision.triangleCount() == oldCollision.triangleCount());
        for (const auto& object : native.objects()) {
            const auto a = collision.floorAt(object.position, 20, 20);
            const auto b = oldCollision.floorAt(object.position, 20, 20);
            REQUIRE(a.has_value() == b.has_value());
            if (a && b) {
                REQUIRE(a->object == b->object);
                REQUIRE(a->objectFlags == b->objectFlags);
                REQUIRE(a->y == b->y);
            }
        }
    }
}

TEST_CASE("native realm WAD data preserves level and camera selections",
          "[native-assets][assets]") {
    const auto directory = test::assetOrSkip("WDATA");
    usize realms = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != ".wad" && entry.path().extension() != ".WAD") {
            continue;
        }
        CAPTURE(entry.path());
        WorldData data;
        REQUIRE(data.load(entry.path()));
        const auto parsed = formats::WorldDataFile::parse(readFile(entry.path()));
        REQUIRE(data.realm() == parsed.realm);
        REQUIRE(data.prefix() == parsed.prefix);
        REQUIRE(data.levels().size() == parsed.levels.size());
        for (usize i = 0; i < parsed.levels.size(); ++i) {
            const auto& a = data.levels()[i];
            const auto& b = parsed.levels[i];
            REQUIRE(a.name == b.name);
            REQUIRE(a.title == b.title);
            REQUIRE(a.flags == b.flags);
            REQUIRE(a.cameraIndex == b.cameraIndex);
            REQUIRE(a.audioIndex == b.audioIndex);
            REQUIRE(a.rune == b.rune);
            REQUIRE(a.legend == b.legend);
            REQUIRE(a.shopMaxima == b.shopMaxima);
            REQUIRE(a.lightColor == b.lightColor);
            REQUIRE(a.lightDirection == b.lightDirection);
            REQUIRE(a.lightIntensity == b.lightIntensity);
            REQUIRE(a.ambient == b.ambient);
            REQUIRE(a.musicVolume == b.musicVolume);
            REQUIRE(a.soundVolume == b.soundVolume);
            if (b.cameraIndex >= 0) {
                const auto* camera = data.camera(b.cameraIndex);
                REQUIRE(camera);
                REQUIRE(camera->attention ==
                        parsed.cameras[static_cast<usize>(b.cameraIndex)].attention);
            }
            if (b.audioIndex >= 0) {
                const auto* audio = data.audio(b.audioIndex);
                REQUIRE(audio);
                REQUIRE(audio->stream == parsed.audio[static_cast<usize>(b.audioIndex)].stream);
            }
        }
        ++realms;
    }
    REQUIRE(realms == 14); // Includes SECRET and TEST, not only the tower's realms.
}

TEST_CASE("native world and realm failures do not fall through to stale manifests",
          "[native-assets]") {
    const auto directory = test::scratchDirectory("native-world-errors");
    writeTextFile(directory / "world.json", R"({"objects":[{"name":"STALE","position":[0,0,0]}]})");
    writeFile(directory / "WORLDS.PS2", std::array<u8, 1>{0});
    WorldLayout layout;
    REQUIRE_FALSE(layout.load(directory));
    REQUIRE_FALSE(layout.loaded());
    writeTextFile(directory / "tower.json", R"({"levels":[{"name":"STALE"}]})");
    writeFile(directory / "TOWER.WAD", std::array<u8, 1>{0});
    WorldData data;
    REQUIRE_FALSE(data.load(directory / "tower.json"));
    REQUIRE_FALSE(data.loaded());
    REQUIRE(data.prefix().empty());
}

TEST_CASE("retail archive census accepts current formats and rejects the legacy demo format",
          "[native-assets][assets]") {
    const auto root = test::assetOrSkip("STATIC/objects.ngc").parent_path().parent_path();
    usize archives = 0;
    usize animations = 0;
    usize worlds = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        const auto name = entry.path().filename().string();
        CAPTURE(entry.path());
        if (name == "objects.ngc" || name == "OBJECTS.NGC") {
            if (toLowerAscii(entry.path().lexically_relative(root).generic_string()) ==
                "items/demo/objects.ngc") {
                // This v4 archive also predates the export reader's supported v12/v13.
                // Preserve an explicit refusal; do not silently skip other unknown formats.
                REQUIRE(readU32LE(readFile(entry.path()), 64) == 0xF00B0004U);
                ModelSet legacy;
                REQUIRE_FALSE(legacy.load(entry.path().parent_path()));
                continue;
            }
            formats::ModelArchive parsed;
            REQUIRE_NOTHROW(parsed = formats::ModelArchive::parse(readFile(entry.path())));
            ModelSet models;
            TextureSet textures;
            REQUIRE(models.load(entry.path().parent_path()) == !parsed.objects().empty());
            REQUIRE(models.size() == parsed.objects().size());
            REQUIRE(textures.load(entry.path().parent_path()) == !parsed.bitmaps().empty());
            REQUIRE(textures.size() == parsed.bitmaps().size());
            ++archives;
        } else if (name == "anim.ps2" || name == "ANIM.PS2") {
            formats::AnimationFile parsed;
            REQUIRE_NOTHROW(parsed = formats::AnimationFile::parse(readFile(entry.path())));
            AnimationSet trees;
            REQUIRE(trees.load(entry.path().parent_path()) ==
                    (!parsed.trees.empty() || !parsed.textureAnimations.empty()));
            REQUIRE(trees.size() == parsed.trees.size());
            REQUIRE(trees.particleTemplates().size() == parsed.particles.size());
            ++animations;
        } else if (name == "worlds.ps2" || name == "WORLDS.PS2") {
            WorldLayout layout;
            WorldCollision collision;
            REQUIRE(layout.load(entry.path().parent_path()));
            REQUIRE(collision.load(entry.path().parent_path(), layout));
            ++worlds;
        }
    }
    CAPTURE(archives, animations, worlds);
    REQUIRE(archives > 300);
    REQUIRE(animations > 100);
    REQUIRE(worlds > 40);
}
} // namespace
