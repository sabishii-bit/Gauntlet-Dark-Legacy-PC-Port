#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <stb_image_write.h>

#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/core/Types.h"
#include "engine/io/AssetLocator.h"
#include "engine/io/File.h"
#include "engine/render/Image.h"

#include "formats/AnimationTree.h"
#include "formats/AudioRom.h"
#include "formats/CritterWad.h"
#include "formats/FontFile.h"
#include "formats/GcTexture.h"
#include "formats/GeometryStream.h"
#include "formats/IcoWriter.h"
#include "formats/JsonWriter.h"
#include "formats/ModelArchive.h"
#include "formats/ObjWriter.h"
#include "formats/PlayerDataWad.h"
#include "formats/WorldDataWad.h"
#include "formats/SoundBank.h"
#include "formats/TextRom.h"
#include "formats/TplFile.h"
#include "formats/WavWriter.h"
#include "formats/WorldFile.h"

namespace {

using namespace gdl;
using namespace gdl::formats;

void writeParticleTemplate(JsonWriter& json, const ParticleTemplateRecord& particle);

void print(std::string_view text) {
    std::fputs(text.data(), stdout);
    std::fputc('\n', stdout);
}

struct Summary {
    u32 archives = 0;
    u32 textures = 0;
    u32 models = 0;
    u32 animations = 0;
    u32 fonts = 0;
    u32 classes = 0;
    u32 critters = 0;
    u32 worlds = 0;
    u32 realms = 0;
    u32 skippedLevels = 0;
    u32 textRoms = 0;
    u32 cardImages = 0;
    u32 banks = 0;
    u32 samples = 0;
    u32 failures = 0;
};

std::string fileSafe(std::string_view name) {
    std::string out;
    for (const char c : name) {
        const auto uc = static_cast<unsigned char>(c);
        out.push_back(std::isalnum(uc) != 0 || c == '_' || c == '-' ? c : '_');
    }
    return out.empty() ? "unnamed" : out;
}

void writePng(const std::filesystem::path& path, const Image& image) {
    const std::string name = path.string();
    if (stbi_write_png(name.c_str(), static_cast<int>(image.width), static_cast<int>(image.height),
                       4, image.pixels.data(), static_cast<int>(image.rowBytes())) == 0) {
        throw FileError(std::format("cannot write {}", name));
    }
}

/** Names for every bitmap index: the def name, or the previous named bitmap plus a frame number. */
std::vector<std::string> bitmapNames(const ModelArchive& archive) {
    std::vector<std::string> names(archive.bitmaps().size());
    for (const ArchiveBitmapDef& def : archive.bitmapDefs()) {
        if (def.textureIndex < names.size() && names[def.textureIndex].empty()) {
            names[def.textureIndex] = normalizeAssetName(def.name);
        }
    }
    std::string base = "UNNAMED";
    u32 frame = 0;
    for (std::string& name : names) {
        if (!name.empty()) {
            base = name;
            frame = 0;
            continue;
        }
        name = std::format("{}+{}", base, ++frame);
    }
    return names;
}

/** Writes the archive's animation trees: hierarchy, object names and every sequence's keys. */
void unpackAnimations(const AssetLocator& locator, const std::filesystem::path& outDir,
                      Summary& summary) {
    const auto animPath = locator.find("anim.ps2");
    if (!animPath.has_value()) {
        return;
    }
    try {
        std::filesystem::create_directories(outDir);
        const AnimationFile file = AnimationFile::parse(readFile(*animPath));
        JsonWriter json;
        json.beginObject();
        json.key("textureAnimations").beginArray();
        for (const TextureAnimation& animation : file.textureAnimations) {
            json.beginObject();
            json.key("name").value(animation.name);
            json.key("frameName").value(animation.frameName);
            json.key("texture").value(animation.texture);
            json.key("source").value(animation.source);
            json.key("frames").value(animation.frames);
            json.key("start").value(animation.start);
            json.key("rate").value(animation.rate);
            json.key("offset").value(animation.offset);
            json.key("flag").value(static_cast<int>(animation.flag));
            json.key("scrollIndex").value(static_cast<int>(animation.scrollIndex));
            json.endObject();
        }
        json.endArray();
        json.key("particles").beginArray();
        for (const ParticleTemplateRecord& particle : file.particles) {
            writeParticleTemplate(json, particle);
        }
        json.endArray();
        json.key("trees").beginArray();
        for (const TreeDefinition& tree : file.trees) {
            json.beginObject();
            json.key("name").value(tree.name);
            json.key("prefix").value(tree.prefix);
            json.key("sequences").beginArray();
            for (const TreeSequence& sequence : tree.sequences) {
                json.beginObject();
                json.key("name").value(sequence.name);
                json.key("frames").value(static_cast<s64>(sequence.frameCount));
                json.key("frameRate").value(static_cast<s64>(sequence.frameRate));
                json.key("repeats").value(sequence.repeats);
                json.key("fixesPosition").value(sequence.fixesPosition);
                json.key("flags").value(u32{sequence.flags});
                json.key("tracks").beginArray();
                for (const NodeTrack& track : sequence.tracks) {
                    json.beginObject();
                    json.key("node").value(track.node);
                    json.key("flags").value(u32{track.flags});
                    json.key("frames").numbers(track.frames);
                    json.key("values").numbers(track.values);
                    json.endObject();
                }
                json.endArray();
                json.endObject();
            }
            json.endArray();
            json.key("nodes").beginArray();
            for (const TreeNode& node : tree.nodes) {
                json.beginObject();
                json.key("name").value(node.name);
                json.key("object").value(node.object);
                json.key("type").value(static_cast<s64>(node.type));
                json.key("flags").value(u32{node.flags});
                json.key("objectFlags").value(node.objectFlags);
                json.key("parent").value(static_cast<s64>(node.parent));
                if (node.particle >= 0) {
                    json.key("particle").value(node.particle);
                    json.key("direction").beginArray();
                    json.value(static_cast<f64>(node.direction.x));
                    json.value(static_cast<f64>(node.direction.y));
                    json.value(static_cast<f64>(node.direction.z));
                    json.endArray();
                }
                if (!node.objectFrames.empty()) {
                    json.key("objectFrames").beginArray();
                    for (const TreeNode::ObjectFrames& run : node.objectFrames) {
                        json.beginObject();
                        json.key("object").value(run.object);
                        json.key("start").value(static_cast<s64>(run.start));
                        json.key("frames").value(static_cast<s64>(run.frames));
                        json.endObject();
                    }
                    json.endArray();
                }
                json.key("position").beginArray();
                json.value(static_cast<f64>(node.position.x));
                json.value(static_cast<f64>(node.position.y));
                json.value(static_cast<f64>(node.position.z));
                json.endArray();
                json.endObject();
            }
            json.endArray();
            json.endObject();
            ++summary.animations;
        }
        json.endArray();
        json.endObject();
        writeTextFile(outDir / "animations.json", json.take());
    } catch (const std::exception& e) {
        ++summary.failures;
        print(std::format("  animations: {}", e.what()));
    }
}

void unpackArchive(const std::filesystem::path& directory, const std::filesystem::path& outDir,
                   Summary& summary) {
    const AssetLocator locator(directory);
    const auto objectsPath = locator.find("objects.ngc");
    const auto texturesPath = locator.find("textures.ngc");
    if (!objectsPath.has_value()) {
        // A folder of animations alone, like a class's ANIM folder, still has trees to write.
        if (locator.find("anim.ps2").has_value()) {
            print(std::format("animations {}", directory.filename().string()));
            unpackAnimations(locator, outDir, summary);
        }
        return;
    }
    const std::string label = directory.filename().string();
    print(std::format("archive {}", label));

    const std::vector<u8> objectsFile = readFile(*objectsPath);
    const ModelArchive archive = ModelArchive::parse(objectsFile);
    std::vector<u8> texturesFile;
    if (texturesPath.has_value()) {
        texturesFile = readFile(*texturesPath);
    }
    std::filesystem::create_directories(outDir / "textures");

    const std::vector<std::string> names = bitmapNames(archive);
    JsonWriter textures;
    textures.beginObject();
    textures.key("source").value(archive.sourceDirectory());
    textures.key("defs").beginArray();
    for (const ArchiveBitmapDef& def : archive.bitmapDefs()) {
        textures.beginObject();
        textures.key("name").value(normalizeAssetName(def.name));
        textures.key("index").value(u32{def.textureIndex});
        textures.key("width").value(u32{def.width});
        textures.key("height").value(u32{def.height});
        textures.endObject();
    }
    textures.endArray();
    textures.key("bitmaps").beginArray();
    for (usize i = 0; i < archive.bitmaps().size(); ++i) {
        const ArchiveBitmap& bitmap = archive.bitmaps()[i];
        const std::string file = std::format("textures/{:03}_{}.png", i, fileSafe(names[i]));
        textures.beginObject();
        textures.key("index").value(static_cast<u64>(i));
        textures.key("name").value(names[i]);
        textures.key("file").value(file);
        textures.key("width").value(u32{bitmap.width});
        textures.key("height").value(u32{bitmap.height});
        textures.key("format").value(u32{bitmap.format});
        textures.key("flags").value(u32{bitmap.flags});
        textures.key("halfResolution").value((bitmap.flags & bitmap_flags::kHalfResolution) != 0);
        textures.key("clampU").value((bitmap.flags & bitmap_flags::kClampU) != 0);
        textures.key("clampV").value((bitmap.flags & bitmap_flags::kClampV) != 0);
        textures.key("hasAlpha").value((bitmap.flags & bitmap_flags::kHasAlpha) != 0);
        textures.key("frames").value(u32{bitmap.frameCount});
        textures.key("mipmaps").value(u32{bitmap.mipmapCount});
        textures.key("lodK").value(static_cast<s64>(bitmap.lodK));
        textures.endObject();
        try {
            if ((bitmap.flags & bitmap_flags::kInvalid) != 0 || texturesFile.empty()) {
                continue;
            }
            Image image = decodeGcTexture(bitmap, texturesFile);
            image.bleedIntoTransparent();
            writePng(outDir / file, image);
            ++summary.textures;
        } catch (const std::exception& e) {
            ++summary.failures;
            print(std::format("  bitmap {} ({}): {}", i, names[i], e.what()));
        }
    }
    textures.endArray();
    textures.endObject();
    writeTextFile(outDir / "textures.json", textures.take());

    std::filesystem::create_directories(outDir / "models");
    JsonWriter objects;
    objects.beginObject();
    objects.key("source").value(archive.sourceDirectory());
    objects.key("objects").beginArray();
    for (usize i = 0; i < archive.objects().size(); ++i) {
        const ArchiveObject& object = archive.objects()[i];
        objects.beginObject();
        objects.key("index").value(static_cast<u64>(i));
        std::string name;
        for (const ArchiveObjectDef& def : archive.objectDefs()) {
            if (def.objectIndex == static_cast<s16>(i)) {
                name = normalizeAssetName(def.name);
                objects.key("name").value(name);
                break;
            }
        }
        try {
            Mesh mesh;
            for (const ArchiveSubObject& sub : object.subObjects) {
                decodeGeometryStream(sub.geometry, sub.textureIndex, sub.lightmapIndex, mesh);
            }
            const std::string file = std::format("models/{:03}_{}.obj", i, fileSafe(name));
            writeTextFile(outDir / file, encodeObj(mesh, name.empty() ? "unnamed" : name));
            objects.key("file").value(file);
            objects.key("meshTriangles").value(static_cast<u64>(mesh.triangleCount()));
            ++summary.models;
        } catch (const std::exception& e) {
            ++summary.failures;
            print(std::format("  object {} ({}): {}", i, name, e.what()));
        }
        objects.key("boundingRadius").value(static_cast<f64>(object.boundingRadius));
        objects.key("flags").value(object.flags);
        objects.key("vertices").value(object.vertexCount);
        objects.key("triangles").value(object.triangleCount);
        objects.key("subObjects").beginArray();
        for (const ArchiveSubObject& sub : object.subObjects) {
            objects.beginObject();
            objects.key("texture").value(u32{sub.textureIndex});
            objects.key("lightmap").value(u32{sub.lightmapIndex});
            objects.key("lodK").value(static_cast<s64>(sub.lodK));
            objects.key("quadwords").value(u32{sub.quadwordCount});
            objects.endObject();
        }
        objects.endArray();
        objects.endObject();
    }
    objects.endArray();
    objects.endObject();
    writeTextFile(outDir / "objects.json", objects.take());
    unpackAnimations(locator, outDir, summary);
    ++summary.archives;
}

void unpackFont(const std::filesystem::path& file, const std::filesystem::path& outDir,
                Summary& summary) {
    const FontFile font = FontFile::parse(readFile(file));
    JsonWriter json;
    json.beginObject();
    json.key("height").value(font.height);
    json.key("glyphs").beginArray();
    for (const FontGlyph& glyph : font.glyphs) {
        json.beginObject();
        json.key("code").value(glyph.code);
        json.key("width").value(glyph.width);
        json.key("x").value(glyph.x);
        json.key("y").value(glyph.y);
        json.endObject();
    }
    json.endArray();
    json.endObject();
    std::filesystem::create_directories(outDir);
    writeTextFile(outDir / (toLowerAscii(file.stem().string()) + ".json"), json.take());
    ++summary.fonts;
}

/** Writes every bank's samples as WAV files plus a manifest of the sounds that sequence them. */
/** A bank's sound as its manifest names it. */
struct BankSoundEntry {
    std::string name;
    s64 id = -1;
    f32 duration = -1.0f;
};

/** Writes one bank's clips and manifest; sounds past `entries` (a bank the audio directory
 * does not name) are numbered after the bank, each as long as its clips. */
void writeBank(const std::filesystem::path& bankDir, std::string_view bankName,
               const SoundBank& sounds, std::vector<BankSoundEntry> entries, Summary& summary) {
    std::filesystem::create_directories(bankDir / "samples");
    std::vector<std::vector<s16>> pcm;
    pcm.reserve(sounds.samples.size());
    for (const BankSample& sample : sounds.samples) {
        pcm.push_back(decodeBankSample(sample));
    }
    for (usize i = entries.size(); i < sounds.calls.size(); ++i) {
        BankSoundEntry entry;
        entry.name = std::format("{}_{:02}", bankName, i);
        f32 seconds = 0.0f;
        bool loops = false;
        for (const SoundStep& step : sounds.calls[i].steps) {
            if (step.sample < pcm.size() && sounds.samples[step.sample].sampleRate > 0) {
                seconds += static_cast<f32>(pcm[step.sample].size()) /
                           static_cast<f32>(sounds.samples[step.sample].sampleRate);
            }
            loops = loops || step.loopBack;
        }
        entry.duration = loops ? -1.0f : seconds;
        entries.push_back(std::move(entry));
    }

    JsonWriter json;
    json.beginObject();
    json.key("bank").value(bankName);
    json.key("sounds").beginArray();
    for (usize i = 0; i < entries.size(); ++i) {
        const BankSoundEntry& entry = entries[i];
        json.beginObject();
        json.key("index").value(static_cast<u64>(i));
        json.key("name").value(entry.name);
        json.key("id").value(entry.id);
        json.key("duration").value(static_cast<f64>(entry.duration));
        if (i < sounds.calls.size()) {
            const SoundCall& call = sounds.calls[i];
            json.key("volume").value(u32{call.volume});
            json.key("duck").value(u32{call.duck});
            json.key("priority").value(u32{call.priority});
            json.key("sequence").beginArray();
            for (const SoundStep& step : call.steps) {
                json.beginObject();
                json.key("sample").value(u32{step.sample});
                json.key("loopStart").value(step.loopStart);
                json.key("loopBack").value(step.loopBack);
                json.endObject();
            }
            json.endArray();
        }
        json.endObject();
    }
    json.endArray();
    json.key("samples").beginArray();
    for (usize i = 0; i < sounds.samples.size(); ++i) {
        const BankSample& sample = sounds.samples[i];
        const std::string file = std::format("samples/{:03}.wav", i);
        writeFile(bankDir / file, encodeWav(pcm[i], sample.sampleRate, 1));
        json.beginObject();
        json.key("index").value(static_cast<u64>(i));
        json.key("name").value(sample.name);
        json.key("file").value(file);
        json.key("sampleRate").value(sample.sampleRate);
        json.key("frames").value(static_cast<u64>(pcm[i].size()));
        json.key("loops").value(sample.loops);
        json.key("loopStart").value(sample.loopStart);
        json.key("loopEnd").value(sample.loopEnd);
        json.endObject();
        ++summary.samples;
    }
    json.endArray();
    json.endObject();
    writeTextFile(bankDir / "sounds.json", json.take());
    ++summary.banks;
}

void unpackAudio(const std::filesystem::path& directory, const std::filesystem::path& outDir,
                 Summary& summary) {
    const AssetLocator locator(directory);
    const auto romPath = locator.find("audatps2.rom");
    if (!romPath.has_value()) {
        print("audio: AUDATPS2.ROM not found");
        return;
    }
    const AudioRom rom = AudioRom::parse(readFile(*romPath));
    std::vector<std::string> named; ///< the bank files the directory names
    for (const AudioRomBank& bank : rom.banks) {
        named.push_back(normalizeAssetName(bank.file));
        const auto bankPath = locator.find(bank.file + ".vbk");
        if (!bankPath.has_value()) {
            print(std::format("audio: bank {} has no .vbk file", bank.name));
            continue;
        }
        print(std::format("audio bank {}", bank.name));
        try {
            const SoundBank sounds = SoundBank::parse(readFile(*bankPath));
            std::vector<BankSoundEntry> entries;
            for (u32 i = 0; i < bank.soundCount; ++i) {
                const AudioRomSound& entry = rom.sounds[bank.firstSound + i];
                entries.push_back(BankSoundEntry{entry.name, entry.id, entry.duration});
            }
            writeBank(outDir / bank.name, bank.name, sounds, std::move(entries), summary);
        } catch (const std::exception& e) {
            ++summary.failures;
            print(std::format("  bank {}: {}", bank.name, e.what()));
        }
    }
    // Banks the directory does not name still ship on the disc; their sounds are numbered.
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (toLowerAscii(entry.path().extension().string()) != ".vbk") {
            continue;
        }
        const std::string name = normalizeAssetName(entry.path().stem().string());
        if (std::ranges::find(named, name) != named.end()) {
            continue;
        }
        print(std::format("audio bank {} (unnamed by the directory)", name));
        try {
            const SoundBank sounds = SoundBank::parse(readFile(entry.path()));
            writeBank(outDir / name, name, sounds, {}, summary);
        } catch (const std::exception& e) {
            ++summary.failures;
            print(std::format("  bank {}: {}", name, e.what()));
        }
    }
}

void unpackTextRom(const std::filesystem::path& file, const std::filesystem::path& outDir,
                   Summary& summary) {
    const TextRom rom = TextRom::parse(readFile(file));
    JsonWriter json;
    json.beginObject();
    json.key("fonts").beginArray();
    for (const std::string& font : rom.fonts) {
        json.value(font);
    }
    json.endArray();
    json.key("messages").beginArray();
    for (const TextMessage& message : rom.messages) {
        json.beginObject();
        json.key("name").value(message.name);
        json.key("font").value(message.font);
        json.key("scale").value(static_cast<f64>(message.scale));
        json.key("shadowScale").value(static_cast<f64>(message.shadowScale));
        json.key("lines").beginArray();
        for (const std::string& line : message.lines) {
            json.value(line);
        }
        json.endArray();
        json.endObject();
    }
    json.endArray();
    json.key("lists").beginArray();
    for (const TextMessageList& list : rom.lists) {
        json.beginObject();
        json.key("name").value(list.name);
        json.key("messages").beginArray();
        for (const u32 index : list.messages) {
            json.value(index);
        }
        json.endArray();
        json.endObject();
    }
    json.endArray();
    json.endObject();
    std::filesystem::create_directories(outDir);
    writeTextFile(outDir / (toLowerAscii(file.stem().string()) + ".json"), json.take());
    ++summary.textRoms;
}

/** Writes one particle template as the manifests carry it. */
void writeParticleTemplate(JsonWriter& json, const ParticleTemplateRecord& particle) {
    const auto vec3 = [&](const Vec3& v) {
        json.beginArray();
        json.value(static_cast<f64>(v.x)).value(static_cast<f64>(v.y));
        json.value(static_cast<f64>(v.z));
        json.endArray();
    };
    json.beginObject();
    json.key("id").value(std::string(1, particle.id));
    json.key("preset").value(u32{particle.preset});
    json.key("flags").value(particle.flags);
    json.key("flagMask").value(particle.flagMask);
    json.key("enables").value(particle.enables);
    json.key("maxParticles").value(particle.maxParticles);
    json.key("maxDirections").value(particle.maxDirections);
    json.key("maxPositions").value(particle.maxPositions);
    json.key("emitterLife").numbers(particle.emitterLife);
    json.key("particleLife").numbers(particle.particleLife);
    json.key("angle").value(particle.angle);
    json.key("textureCount").value(particle.textureCount);
    json.key("texture").value(particle.texture);
    json.key("direction");
    vec3(particle.direction);
    json.key("volume");
    vec3(particle.volume);
    json.key("rate").numbers(particle.rate);
    json.key("rateRandom").value(particle.rateRandom);
    json.key("gravity").value(particle.gravity);
    json.key("drag").value(particle.drag);
    json.key("speed").value(particle.speed);
    json.key("rgba").beginArray();
    for (const u32 value : particle.rgba) {
        json.value(value);
    }
    json.endArray();
    json.key("width").numbers(particle.width);
    json.key("delay").value(particle.delay);
    json.endObject();
}

/** The collision triangles grouped by the object that owns them, in world space. */
std::string collisionJson(const WorldFile& world) {
    JsonWriter json;
    json.beginObject();
    json.key("objects").beginArray();
    for (usize i = 0; i < world.objects.size(); ++i) {
        const WorldObjectRecord& object = world.objects[i];
        if (object.collisionTriangleCount <= 0 || object.collisionTriangleIndex < 0) {
            continue;
        }
        const auto first = static_cast<usize>(object.collisionTriangleIndex);
        const auto count = static_cast<usize>(object.collisionTriangleCount);
        if (first + count > world.collision.size()) {
            continue;
        }
        json.beginObject();
        json.key("object").value(static_cast<u64>(i));
        json.key("normals").beginArray();
        for (usize t = first; t < first + count; ++t) {
            const Vec3& n = world.collision[t].normal;
            json.value(static_cast<f64>(n.x)).value(static_cast<f64>(n.y));
            json.value(static_cast<f64>(n.z));
        }
        json.endArray();
        json.key("vertices").beginArray();
        for (usize t = first; t < first + count; ++t) {
            for (const Vec3& v : world.collision[t].vertices) {
                json.value(static_cast<f64>(v.x)).value(static_cast<f64>(v.y));
                json.value(static_cast<f64>(v.z));
            }
        }
        json.endArray();
        json.endObject();
    }
    json.endArray();
    json.endObject();
    return json.take();
}

/** Writes a level's placed objects, marker points and collision triangles. */
void unpackWorld(const AssetLocator& files, const std::filesystem::path& outDir,
                 Summary& summary) {
    const auto worldPath = files.find("worlds.ps2");
    if (!worldPath.has_value()) {
        return;
    }
    try {
        const WorldFile world = WorldFile::parse(readFile(*worldPath));
        JsonWriter json;
        const auto vec3 = [&](const Vec3& v) {
            json.beginArray();
            json.value(static_cast<f64>(v.x)).value(static_cast<f64>(v.y));
            json.value(static_cast<f64>(v.z));
            json.endArray();
        };
        json.beginObject();
        json.key("bounds").beginObject();
        json.key("min");
        vec3(world.minBounds);
        json.key("max");
        vec3(world.maxBounds);
        json.endObject();
        json.key("collisionTriangles").value(world.collisionTriangleCount);
        json.key("itemInfos").value(world.itemInfoCount);
        json.key("itemInstances").value(world.itemInstanceCount);
        json.key("particleSystems").value(world.particleSystemCount);
        json.key("objects").beginArray();
        for (const WorldObjectRecord& object : world.objects) {
            json.beginObject();
            json.key("name").value(object.name);
            json.key("position");
            vec3(object.position);
            json.key("flags").value(object.flags);
            json.key("objectFlags").value(object.objectFlags);
            json.key("next").value(static_cast<s64>(object.nextIndex));
            json.key("child").value(static_cast<s64>(object.childIndex));
            json.key("radius").value(static_cast<f64>(object.radius));
            if (object.noCollision) {
                json.key("noCollision").value(true);
            }
            json.endObject();
        }
        json.endArray();
        json.key("animations").beginArray();
        for (const WorldAnimationRecord& animation : world.animations) {
            json.beginObject();
            json.key("object").value(animation.objectIndex);
            json.key("frames").value(animation.frameCount);
            json.key("state").value(animation.state);
            json.key("start").value(animation.startFrame);
            json.key("track").beginObject();
            json.key("flags").value(u32{animation.track.flags});
            json.key("frames").numbers(animation.track.frames);
            json.key("values").numbers(animation.track.values);
            json.endObject();
            json.endObject();
        }
        json.endArray();
        json.key("itemInfos").beginArray();
        for (const ItemInfoRecord& info : world.itemInfos) {
            json.beginObject();
            json.key("type").value(info.type);
            json.key("subtype").value(info.subtype);
            json.key("name").value(info.name);
            json.key("collisionType").value(static_cast<s64>(info.collisionType));
            json.key("collisionFlags").value(static_cast<s64>(info.collisionFlags));
            json.key("radius").value(info.radius);
            json.key("height").value(info.height);
            json.key("xSize").value(info.xSize);
            json.key("zSize").value(info.zSize);
            json.key("collisionOffset");
            vec3(info.collisionOffset);
            json.key("objectFlags").value(info.objectFlags);
            json.key("properties").value(info.properties);
            json.key("value").value(static_cast<s64>(info.value));
            json.key("armor").value(static_cast<s64>(info.armor));
            json.key("hitPoints").value(static_cast<s64>(info.hitPoints));
            json.key("activeType").value(static_cast<s64>(info.activeType));
            json.key("activeOff").value(static_cast<s64>(info.activeOff));
            json.key("activeOn").value(static_cast<s64>(info.activeOn));
            if (!info.choices.empty()) {
                json.key("choices").beginArray();
                for (const s16 choice : info.choices) {
                    json.value(static_cast<s64>(choice));
                }
                json.endArray();
            }
            json.endObject();
        }
        json.endArray();
        json.key("itemInstances").beginArray();
        for (const ItemInstanceRecord& instance : world.itemInstances) {
            json.beginObject();
            json.key("info").value(static_cast<s64>(instance.info));
            json.key("minPlayers").value(static_cast<s64>(instance.minPlayers));
            json.key("flags").value(u32{instance.flags});
            json.key("triangleIndex").value(static_cast<s64>(instance.triangleIndex));
            json.key("triangleCount").value(static_cast<s64>(instance.triangleCount));
            json.key("name").value(instance.name);
            json.key("position");
            vec3(instance.position);
            json.key("rotation");
            vec3(instance.rotation);
            json.key("params").beginArray();
            for (const u8 value : instance.params) {
                json.value(u32{value});
            }
            json.endArray();
            json.endObject();
        }
        json.endArray();
        json.key("particles").beginArray();
        for (const ParticleTemplateRecord& particle : world.particles) {
            writeParticleTemplate(json, particle);
        }
        json.endArray();
        json.key("locators").beginArray();
        for (const WorldLocatorRecord& locator : world.locators) {
            json.beginObject();
            json.key("type").value(locatorKindName(locator.kind));
            json.key("delay").value(u32{locator.delay});
            json.key("next").value(u32{locator.next});
            json.key("position");
            vec3(locator.position);
            json.key("rotation");
            vec3(locator.rotation);
            json.endObject();
        }
        json.endArray();
        json.endObject();
        std::filesystem::create_directories(outDir);
        writeTextFile(outDir / "world.json", json.take());
        writeTextFile(outDir / "collision.json", collisionJson(world));
        ++summary.worlds;
    } catch (const std::exception& e) {
        ++summary.failures;
        print(std::format("  world: {}", e.what()));
    }
}

/**
 * The player folder: one archive per class and costume (`WAR/BLU`), the costumes worn from
 * level 10 upwards (`WAR/BLU10`) only when `tiers` is set, the select-screen figures, and each
 * class's `ANIM` folder holding the sequences every costume of the class plays.
 */
void unpackPlayers(const std::filesystem::path& directory, const std::filesystem::path& outDir,
                   bool tiers, Summary& summary) {
    std::vector<std::filesystem::path> classes;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_directory()) {
            classes.push_back(entry.path());
        }
    }
    std::ranges::sort(classes);
    u32 skipped = 0;
    for (const auto& classDirectory : classes) {
        std::vector<std::filesystem::path> costumes;
        for (const auto& entry : std::filesystem::directory_iterator(classDirectory)) {
            if (entry.is_directory()) {
                costumes.push_back(entry.path());
            }
        }
        std::ranges::sort(costumes);
        for (const auto& costume : costumes) {
            const std::string name = normalizeAssetName(costume.filename().string());
            const bool tier = name.size() >= 2 && std::isdigit(static_cast<unsigned char>(name.back())) != 0 &&
                              std::isdigit(static_cast<unsigned char>(name[name.size() - 2])) != 0;
            if (tier && !tiers) {
                ++skipped;
                continue;
            }
            const std::string className = normalizeAssetName(classDirectory.filename().string());
            unpackArchive(costume, outDir / className / name, summary);
        }
    }
    if (skipped > 0) {
        print(std::format("PLAYERS: {} levelled costumes skipped; pass --tiers to unpack them",
                          skipped));
    }
}

/**
 * The monster folder: one archive per kind (`GRU`, `RAT`), some with a second or an auxiliary
 * archive beside it (`GRU2`, `GRUAUX`), and the `GENERAL` figure in one folder per realm.
 */
void unpackMonsters(const std::filesystem::path& directory, const std::filesystem::path& outDir,
                    Summary& summary) {
    std::vector<std::filesystem::path> kinds;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_directory()) {
            kinds.push_back(entry.path());
        }
    }
    std::ranges::sort(kinds);
    for (const auto& kind : kinds) {
        const std::string name = normalizeAssetName(kind.filename().string());
        if (AssetLocator(kind).find("objects.ngc")) {
            unpackArchive(kind, outDir / name, summary);
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(kind)) {
            if (entry.is_directory()) {
                const std::string realm = normalizeAssetName(entry.path().filename().string());
                unpackArchive(entry.path(), outDir / name / realm, summary);
            }
        }
    }
}

/** A critter wad: the great creature's type, moves, damages, parts and sounds, as one file. */
void unpackCritter(const std::filesystem::path& file, const std::filesystem::path& outDir,
                   Summary& summary) {
    const CritterFile critter = parseCritterWad(readFile(file));
    JsonWriter json;
    const auto vec = [&](const char* name, const std::array<f32, 3>& v) {
        json.key(name).beginArray();
        for (const f32 axis : v) {
            json.value(static_cast<f64>(axis));
        }
        json.endArray();
    };
    const auto target = [&](const CritterTargetRecord& t) {
        json.key("target").beginObject();
        json.key("minDistance").value(static_cast<f64>(t.minDistance));
        json.key("maxDistance").value(static_cast<f64>(t.maxDistance));
        json.key("yaw").value(static_cast<f64>(t.yaw));
        json.key("minDot").value(static_cast<f64>(t.minDot));
        json.key("minRateScale").value(static_cast<f64>(t.minRateScale));
        json.key("maxRateScale").value(static_cast<f64>(t.maxRateScale));
        json.key("idleGate").value(static_cast<f64>(t.idleGate));
        json.key("maxVertical").value(static_cast<f64>(t.maxVertical));
        json.endObject();
    };
    json.beginObject();
    json.key("name").value(normalizeAssetName(file.stem().string()));
    json.key("descriptors").beginArray();
    for (const CritterDescriptorRecord& d : critter.descriptors) {
        json.beginObject();
        json.key("name").value(d.name);
        json.key("prefix").value(d.prefix);
        json.key("type").value(static_cast<int>(d.type));
        json.endObject();
    }
    json.endArray();
    json.key("types").beginArray();
    for (const CritterTypeRecord& t : critter.types) {
        json.beginObject();
        json.key("suffix").value(t.suffix);
        json.key("rootNode").value(t.rootNode);
        json.key("descriptor").value(static_cast<int>(t.descriptorIndex));
        json.key("subtype").value(static_cast<int>(t.subtype));
        json.key("typeFlags").value(t.typeFlags);
        json.key("radius").value(static_cast<f64>(t.radius));
        json.key("wallRadius").value(static_cast<f64>(t.wallRadius));
        target(t.target);
        vec("defaultPos", t.defaultPos);
        json.key("speed").value(static_cast<f64>(t.speed));
        json.key("floorOffset").value(static_cast<f64>(t.floorOffset));
        json.key("vertDrift").value(static_cast<f64>(t.vertDrift));
        json.key("damageScale").value(static_cast<f64>(t.damageScale));
        json.key("armor").value(static_cast<f64>(t.armor));
        vec("originOffset", t.originOffset);
        json.key("turnLimit").value(static_cast<f64>(t.turnLimit));
        json.key("shieldFlags").value(t.shieldFlags);
        json.key("maxHealth").value(static_cast<f64>(t.maxHealth));
        json.key("expValue").value(static_cast<f64>(t.expValue));
        json.key("wakeThreshold").value(static_cast<f64>(t.wakeThreshold));
        json.key("moveCount").value(static_cast<int>(t.moveCount));
        json.key("moveIndex").value(static_cast<int>(t.moveIndex));
        json.key("patternCount").value(static_cast<int>(t.patternCount));
        json.key("patternIndex").value(static_cast<int>(t.patternIndex));
        json.key("colCount").value(static_cast<int>(t.colCount));
        json.key("colBase").value(static_cast<int>(t.colBase));
        json.key("childIndex").value(static_cast<int>(t.childIndex));
        json.key("parentIndex").value(static_cast<int>(t.parentIndex));
        json.endObject();
    }
    json.endArray();
    json.key("moves").beginArray();
    for (const CritterMoveRecord& m : critter.moves) {
        json.beginObject();
        json.key("type").value(m.type);
        json.key("flags").value(m.flags);
        json.key("priority").value(m.priority);
        json.key("name").value(m.name);
        json.key("anim").value(m.anim);
        json.key("colnode").value(m.colnode);
        json.key("frameStart").value(m.frameStart);
        json.key("frameStart2").value(m.frameStart2);
        json.key("damage0").value(static_cast<int>(m.damage0));
        json.key("damage1").value(static_cast<int>(m.damage1));
        json.key("framePeriod").value(static_cast<f64>(m.framePeriod));
        json.key("frameEnd").value(static_cast<int>(m.frameEnd));
        json.key("frameEnd2").value(static_cast<int>(m.frameEnd2));
        json.key("link").value(static_cast<int>(m.link));
        json.key("interrupt").value(static_cast<int>(m.interrupt));
        json.key("sfx").value(static_cast<int>(m.sfx));
        json.key("sfxFrame").value(static_cast<int>(m.sfxFrame));
        json.key("sfx2").value(static_cast<int>(m.sfx2));
        json.key("sfx2Frame").value(static_cast<int>(m.sfx2Frame));
        target(m.target);
        json.key("cooldown").value(static_cast<f64>(m.cooldown));
        json.key("speed").value(static_cast<f64>(m.speed));
        json.key("turnRate").value(static_cast<f64>(m.turnRate));
        json.key("hold").value(static_cast<f64>(m.hold));
        json.endObject();
    }
    json.endArray();
    json.key("damages").beginArray();
    for (const CritterDamageRecord& d : critter.damages) {
        json.beginObject();
        json.key("type").value(static_cast<int>(d.type));
        json.key("behaviorFlags").value(static_cast<int>(d.behaviorFlags));
        json.key("flags").value(d.flags);
        json.key("radius").value(static_cast<f64>(d.radius));
        json.key("maxDistance").value(static_cast<f64>(d.maxDistance));
        json.key("minDistance").value(static_cast<f64>(d.minDistance));
        json.key("yaw").value(static_cast<f64>(d.yaw));
        json.key("minDot").value(static_cast<f64>(d.minDot));
        json.key("pitch").value(static_cast<f64>(d.pitch));
        vec("offset", d.offset);
        json.key("damage").value(static_cast<f64>(d.damage));
        json.key("sfxIndex").value(static_cast<int>(d.sfxIndex));
        json.key("sfx").value(static_cast<int>(d.sfx));
        json.endObject();
    }
    json.endArray();
    json.key("nodes").beginArray();
    for (const CritterNodeRecord& n : critter.nodes) {
        json.beginObject();
        json.key("nodeName").value(n.nodeName);
        json.key("flags").value(static_cast<int>(n.flags));
        json.key("sfxIndex").value(static_cast<int>(n.sfxIndex));
        json.key("maxTargetDistance").value(static_cast<f64>(n.maxTargetDistance));
        json.key("targetScoreScale").value(static_cast<f64>(n.targetScoreScale));
        vec("position", n.position);
        json.key("radius").value(static_cast<f64>(n.radius));
        json.key("attach").value(n.attach);
        json.key("damageScale").value(static_cast<f64>(n.damageScale));
        json.key("healthScale").value(static_cast<f64>(n.healthScale));
        json.endObject();
    }
    json.endArray();
    json.key("sounds").beginArray();
    for (const CritterSoundRecord& s : critter.sounds) {
        json.beginObject();
        json.key("name").value(s.name);
        json.key("levelFormat").value(s.levelFormat);
        json.endObject();
    }
    json.endArray();
    json.endObject();
    std::filesystem::create_directories(outDir);
    writeTextFile(outDir / (normalizeAssetName(file.stem().string()) + ".json"), json.take());
    ++summary.critters;
}

/** A level folder: its archive plus the world file. */
void unpackLevel(const std::filesystem::path& directory, const std::filesystem::path& outDir,
                 Summary& summary) {
    unpackArchive(directory, outDir, summary);
    unpackWorld(AssetLocator(directory), outDir, summary);
}

void unpackClassData(const std::filesystem::path& file, const std::filesystem::path& outDir,
                     Summary& summary) {
    const PlayerClassRecord record = parsePlayerDataWad(readFile(file));
    JsonWriter json;
    json.beginObject();
    json.key("code").value(normalizeAssetName(file.stem().string()));
    const auto range = [&](const char* name, f32 low, f32 high) {
        json.key(name).beginArray().value(static_cast<f64>(low)).value(static_cast<f64>(high));
        json.endArray();
    };
    range("fight", record.fightMin, record.fightMax);
    range("speed", record.speedMin, record.speedMax);
    range("armor", record.armorMin, record.armorMax);
    range("magic", record.magicMin, record.magicMax);
    json.key("height").value(static_cast<f64>(record.height));
    json.key("width").value(static_cast<f64>(record.width));
    json.key("attachY").value(static_cast<f64>(record.attachY));
    json.key("collisionY").value(static_cast<f64>(record.collisionY));
    json.key("powerupTime").value(static_cast<f64>(record.powerupTime));
    json.key("weaponOffset").beginArray();
    for (const f32 axis : record.weaponOffset) {
        json.value(static_cast<f64>(axis));
    }
    json.endArray();
    json.key("moves").beginObject();
    for (usize move = 0; move < record.moves.size(); ++move) {
        json.key(PlayerClassRecord::kMoveNames[move]).value(static_cast<int>(record.moves[move]));
    }
    json.endObject();
    json.key("moveEffects").beginArray();
    for (const MoveEffectRecord& effect : record.effects) {
        json.beginObject();
        json.key("flags").value(effect.flags);
        json.key("next").value(static_cast<int>(effect.next));
        json.key("tree").value(effect.tree);
        json.key("sound").value(effect.sound);
        json.key("offset").numbers(effect.offset);
        json.key("scale").value(static_cast<f64>(effect.scale));
        json.endObject();
    }
    json.endArray();
    json.key("moveStrikes").beginArray();
    for (const MoveStrikeRecord& strike : record.strikes) {
        json.beginObject();
        json.key("type").value(static_cast<int>(strike.type));
        json.key("flags").value(static_cast<int>(strike.flags));
        json.key("damageType").value(strike.damageType);
        json.key("hitRadius").value(static_cast<f64>(strike.hitRadius));
        json.key("radius").value(static_cast<f64>(strike.radius));
        json.key("delay").value(static_cast<f64>(strike.delay));
        json.key("minTime").value(static_cast<f64>(strike.minTime));
        json.key("maxTime").value(static_cast<f64>(strike.maxTime));
        json.key("angle").value(static_cast<f64>(strike.angle));
        json.key("arc").value(static_cast<f64>(strike.arc));
        json.key("offset").numbers(strike.offset);
        json.key("amount").value(static_cast<f64>(strike.amount));
        json.key("speedMin").value(static_cast<f64>(strike.speedMin));
        json.key("speedMax").value(static_cast<f64>(strike.speedMax));
        json.key("weight").value(static_cast<f64>(strike.weight));
        json.key("effect").value(static_cast<int>(strike.effect));
        json.key("hitEffect").value(static_cast<int>(strike.hitEffect));
        json.key("loopEffect").value(static_cast<int>(strike.loopEffect));
        json.key("next").value(static_cast<int>(strike.next));
        json.key("startFrame").value(static_cast<int>(strike.startFrame));
        json.key("endFrame").value(static_cast<int>(strike.endFrame));
        json.key("help").value(static_cast<int>(strike.help));
        json.endObject();
    }
    json.endArray();
    json.endObject();
    std::filesystem::create_directories(outDir);
    writeTextFile(outDir / (normalizeAssetName(file.stem().string()) + ".json"), json.take());
    ++summary.classes;
}

/**
 * The memory-card art beside the game folder on the disc: every frame of the icon and the
 * banner as PNG, plus the icon's first frame as a Windows .ico at 32 to 256 pixels for the
 * executable and window.
 */
void unpackCardArt(const std::filesystem::path& directory, const std::filesystem::path& outDir,
                   Summary& summary) {
    std::filesystem::create_directories(outDir);
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (toLowerAscii(entry.path().extension().string()) != ".tpl") {
            continue;
        }
        const std::string stem = toLowerAscii(entry.path().stem().string());
        const std::vector<TplImage> images = parseTplFile(readFile(entry.path()));
        for (usize i = 0; i < images.size(); ++i) {
            const std::string name = images.size() == 1 ? std::format("{}.png", stem)
                                                        : std::format("{}{}.png", stem, i);
            writePng(outDir / name, images[i].image);
            ++summary.cardImages;
        }
        if (stem == "icon" && !images.empty()) {
            std::vector<Image> sizes;
            for (u32 factor = 1; factor <= 8; factor *= 2) {
                sizes.push_back(enlargeImage(images[0].image, factor));
            }
            writeFile(outDir / "icon.ico", encodeIco(sizes));
        }
    }
}

/** A realm's data wad: its levels with their light, camera and audio records. */
void unpackWorldData(const std::filesystem::path& file, const std::filesystem::path& outDir,
                     Summary& summary) {
    const WorldDataFile data = WorldDataFile::parse(readFile(file));
    JsonWriter json;
    json.beginObject();
    json.key("realm").value(data.realm);
    json.key("prefix").value(data.prefix);
    json.key("enemies").beginArray();
    for (const WorldEnemyRecord& enemy : data.enemies) {
        json.beginObject();
        json.key("kind").value(enemy.kind);
        json.key("subtype").value(enemy.subtype);
        json.key("stream").value(enemy.stream);
        json.endObject();
    }
    json.endArray();
    json.key("levels").beginArray();
    for (const LevelRecord& level : data.levels) {
        json.beginObject();
        json.key("name").value(level.name);
        json.key("title").value(level.title);
        json.key("flags").value(level.flags);
        json.key("audioBank").value(level.audioBank);
        json.key("movie").value(level.movie);
        json.key("bossType").value(level.bossType);
        json.key("enemyTypes").beginArray();
        for (const s16 row : level.enemyTypes) {
            json.value(static_cast<int>(row));
        }
        json.endArray();
        json.key("cameraIndex").value(static_cast<int>(level.cameraIndex));
        json.key("audioIndex").value(static_cast<int>(level.audioIndex));
        json.key("mapIndex").value(static_cast<int>(level.mapIndex));
        json.key("rune").value(static_cast<int>(level.rune));
        json.key("legend").value(static_cast<int>(level.legend));
        json.key("maxEnemies").value(static_cast<int>(level.maxEnemies));
        json.key("musicVolume").value(level.musicVolume);
        json.key("soundVolume").value(level.soundVolume);
        json.key("tuning").beginObject();
        for (usize i = 0; i < LevelTuningRecord::kCount; ++i) {
            json.key(LevelTuningRecord::kNames[i]).value(level.tuning.values[i]);
        }
        json.endObject();
        json.key("ambient").value(level.ambient);
        json.key("lightDirection").numbers(std::array<f32, 3>{
            level.lightDirection.x, level.lightDirection.y, level.lightDirection.z});
        json.key("lightColor").numbers(
            std::array<f32, 3>{level.lightColor.x, level.lightColor.y, level.lightColor.z});
        json.key("lightIntensity").value(level.lightIntensity);
        json.key("fog").beginObject();
        json.key("type").value(u32{level.fog.type});
        json.key("color").numbers(std::array<u16, 3>{level.fog.color[0], level.fog.color[1],
                                                     level.fog.color[2]});
        json.key("intensity").value(level.fog.intensity);
        json.key("density").value(level.fog.density);
        json.key("min").value(level.fog.min);
        json.key("max").value(level.fog.max);
        json.key("near").value(level.fog.near);
        json.key("far").value(level.fog.far);
        json.endObject();
        json.endObject();
    }
    json.endArray();
    json.key("cameras").beginArray();
    for (const CameraRecord& camera : data.cameras) {
        json.beginObject();
        json.key("direction").value(static_cast<int>(camera.direction));
        json.key("pitchDirection").value(static_cast<int>(camera.pitchDirection));
        json.key("dp").value(camera.dp);
        json.key("minPitch").value(camera.minPitch);
        json.key("maxPitch").value(camera.maxPitch);
        json.key("boundsMin").numbers(
            std::array<f32, 3>{camera.boundsMin.x, camera.boundsMin.y, camera.boundsMin.z});
        json.key("boundsMax").numbers(
            std::array<f32, 3>{camera.boundsMax.x, camera.boundsMax.y, camera.boundsMax.z});
        json.key("limits").value(u32{camera.limits});
        json.key("startEvent").value(u32{camera.startEvent});
        json.key("attentionCamera").value(static_cast<int>(camera.attentionCamera));
        json.key("attention").value(camera.attention);
        json.key("radiusMin").value(camera.radiusMin);
        json.key("radiusMax").value(camera.radiusMax);
        json.key("enemyMax").value(static_cast<int>(camera.enemyMax));
        json.key("specialRadius").value(static_cast<int>(camera.specialRadius));
        json.key("pitchSub").value(camera.pitchSub);
        json.key("pitchMul").value(camera.pitchMul);
        json.key("pitchAdd").value(camera.pitchAdd);
        json.key("distMulAdd").value(camera.distMulAdd);
        json.key("distMulFactor").value(camera.distMulFactor);
        json.key("distMulMin").value(camera.distMulMin);
        json.key("distMulMax").value(camera.distMulMax);
        json.key("smooth").value(camera.smooth);
        json.key("minYaw").value(camera.minYaw);
        json.key("maxYaw").value(camera.maxYaw);
        json.key("bossRadiusMin").value(camera.bossRadiusMin);
        json.key("bossRadiusMax").value(camera.bossRadiusMax);
        json.endObject();
    }
    json.endArray();
    json.key("audio").beginArray();
    for (const AudioRecord& audio : data.audio) {
        json.beginObject();
        json.key("bank").value(audio.bank);
        json.key("stream").value(audio.stream);
        json.key("enterSound").value(static_cast<int>(audio.enterSound));
        json.key("hitSound").value(static_cast<int>(audio.hitSound));
        json.key("nameSound").value(audio.nameSound);
        json.key("areas").value(static_cast<int>(audio.areas));
        json.key("stereo").value(static_cast<int>(audio.stereo));
        json.endObject();
    }
    json.endArray();
    json.key("sounds").beginArray();
    for (const SoundRecord& sound : data.sounds) {
        json.beginObject();
        json.key("name").value(sound.name);
        json.key("volume").value(static_cast<int>(sound.volume));
        json.key("priority").value(static_cast<int>(sound.priority));
        json.endObject();
    }
    json.endArray();
    json.endObject();
    std::filesystem::create_directories(outDir);
    writeTextFile(outDir / (normalizeAssetName(file.stem().string()) + ".json"), json.take());
    ++summary.realms;
}

int run(const std::filesystem::path& assetRoot, const std::filesystem::path& outRoot,
        std::string_view only, bool levels, bool tiers) {
    Summary summary;
    std::vector<std::filesystem::path> directories;
    for (const auto& entry : std::filesystem::directory_iterator(assetRoot)) {
        if (entry.is_directory()) {
            directories.push_back(entry.path());
        }
    }
    std::ranges::sort(directories);

    for (const auto& directory : directories) {
        const std::string name = directory.filename().string();
        const std::string upper = normalizeAssetName(name);
        if (!only.empty() && upper != normalizeAssetName(only) && upper != "LEVELS" &&
            upper != "ITEMS") {
            continue;
        }
        try {
            if (upper == "FONTS") {
                for (const auto& entry : std::filesystem::directory_iterator(directory)) {
                    if (toLowerAscii(entry.path().extension().string()) == ".fnt") {
                        unpackFont(entry.path(), outRoot / "fonts", summary);
                    }
                }
            } else if (upper == "LEVELS") {
                // Every level unpacks to about twenty megabytes, so they are opt-in.
                if (!levels && only.empty()) {
                    print("LEVELS: skipped; pass --levels to unpack the level folders");
                    continue;
                }
                std::vector<std::filesystem::path> levelDirectories;
                for (const auto& entry : std::filesystem::directory_iterator(directory)) {
                    if (entry.is_directory()) {
                        levelDirectories.push_back(entry.path());
                    }
                }
                std::ranges::sort(levelDirectories);
                for (const auto& level : levelDirectories) {
                    const std::string levelName = normalizeAssetName(level.filename().string());
                    if (!only.empty() && normalizeAssetName(only) != "LEVELS" &&
                        levelName != normalizeAssetName(only)) {
                        ++summary.skippedLevels;
                        continue;
                    }
                    unpackLevel(level, outRoot / "LEVELS" / levelName, summary);
                }
            } else if (upper == "ITEMS") {
                // Each level's item set is its own archive, opt-in alongside the levels.
                if (!levels && only.empty()) {
                    print("ITEMS: skipped; pass --levels to unpack the level item sets");
                    continue;
                }
                std::vector<std::filesystem::path> itemDirectories;
                for (const auto& entry : std::filesystem::directory_iterator(directory)) {
                    if (entry.is_directory()) {
                        itemDirectories.push_back(entry.path());
                    }
                }
                std::ranges::sort(itemDirectories);
                for (const auto& items : itemDirectories) {
                    const std::string itemName = normalizeAssetName(items.filename().string());
                    if (!only.empty() && normalizeAssetName(only) != "ITEMS" &&
                        itemName != normalizeAssetName(only)) {
                        ++summary.skippedLevels;
                        continue;
                    }
                    unpackArchive(items, outRoot / "ITEMS" / itemName, summary);
                }
            } else if (upper == "PLAYERS") {
                unpackPlayers(directory, outRoot / "PLAYERS", tiers, summary);
            } else if (upper == "MONSTERS") {
                unpackMonsters(directory, outRoot / "MONSTERS", summary);
            } else if (upper == "WDATA") {
                for (const auto& entry : std::filesystem::directory_iterator(directory)) {
                    if (toLowerAscii(entry.path().extension().string()) == ".wad") {
                        unpackWorldData(entry.path(), outRoot / "wdata", summary);
                    }
                }
            } else if (upper == "CRITTER") {
                for (const auto& entry : std::filesystem::directory_iterator(directory)) {
                    if (toLowerAscii(entry.path().extension().string()) == ".wad") {
                        unpackCritter(entry.path(), outRoot / "critter", summary);
                    }
                }
            } else if (upper == "PDATA") {
                for (const auto& entry : std::filesystem::directory_iterator(directory)) {
                    if (toLowerAscii(entry.path().extension().string()) == ".wad") {
                        unpackClassData(entry.path(), outRoot / "pdata", summary);
                    }
                }
            } else if (upper == "AUDIO") {
                unpackAudio(directory, outRoot / "audio", summary);
            } else if (upper == "TEXT") {
                for (const auto& entry : std::filesystem::directory_iterator(directory)) {
                    if (toLowerAscii(entry.path().extension().string()) == ".rom") {
                        unpackTextRom(entry.path(), outRoot / "text", summary);
                    }
                }
            } else {
                unpackArchive(directory, outRoot / upper, summary);
            }
        } catch (const std::exception& e) {
            ++summary.failures;
            print(std::format("{}: {}", name, e.what()));
        }
    }
    // The memory-card art sits beside the game folder, not inside it.
    if (only.empty() || normalizeAssetName(only) == "CARDDEMO") {
        const std::filesystem::path disc = std::filesystem::absolute(assetRoot).parent_path();
        for (const auto& entry : std::filesystem::directory_iterator(disc)) {
            if (!entry.is_directory() ||
                normalizeAssetName(entry.path().filename().string()) != "CARDDEMO") {
                continue;
            }
            try {
                unpackCardArt(entry.path(), outRoot / "carddemo", summary);
            } catch (const std::exception& e) {
                ++summary.failures;
                print(std::format("carddemo: {}", e.what()));
            }
        }
    }
    print(std::format("{} archives, {} textures, {} models, {} animation trees, {} fonts, "
                      "{} text roms, {} sound banks, {} samples, {} classes, {} critters, {} worlds, "
                      "{} realms, {} card images, {} failures",
                      summary.archives, summary.textures, summary.models, summary.animations,
                      summary.fonts, summary.textRoms, summary.banks, summary.samples,
                      summary.classes, summary.critters, summary.worlds, summary.realms, summary.cardImages,
                      summary.failures));
    return summary.failures == 0 ? 0 : 3;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        std::vector<std::string_view> args;
        const std::span<char*> rawArgs(argv, static_cast<usize>(argc));
        for (const char* arg : rawArgs.subspan(rawArgs.empty() ? 0 : 1)) {
            args.emplace_back(arg);
        }
        std::string_view only;
        bool levels = false;
        bool tiers = false;
        std::vector<std::string_view> positional;
        for (usize i = 0; i < args.size(); ++i) {
            if (args[i] == "--only" && i + 1 < args.size()) {
                only = args[++i];
            } else if (args[i] == "--levels") {
                levels = true;
            } else if (args[i] == "--tiers") {
                tiers = true;
            } else {
                positional.push_back(args[i]);
            }
        }
        if (positional.size() != 2) {
            print("usage: gdlunpack <asset-root> <output-root> [--only <directory|level>] "
                  "[--levels] [--tiers]");
            return 2;
        }
        return run(positional[0], positional[1], only, levels, tiers);
    } catch (const std::exception& e) {
        std::fputs("error: ", stdout);
        std::fputs(e.what(), stdout);
        std::fputc('\n', stdout);
        return 1;
    }
}
