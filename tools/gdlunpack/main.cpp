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
#include "formats/FontFile.h"
#include "formats/GcTexture.h"
#include "formats/GeometryStream.h"
#include "formats/JsonWriter.h"
#include "formats/ModelArchive.h"
#include "formats/ObjWriter.h"
#include "formats/PlayerDataWad.h"
#include "formats/SoundBank.h"
#include "formats/TextRom.h"
#include "formats/WavWriter.h"
#include "formats/WorldFile.h"

namespace {

using namespace gdl;
using namespace gdl::formats;

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
    u32 worlds = 0;
    u32 skippedLevels = 0;
    u32 textRoms = 0;
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
    for (usize i = 0; i < names.size(); ++i) {
        if (!names[i].empty()) {
            base = names[i];
            frame = 0;
            continue;
        }
        names[i] = std::format("{}+{}", base, ++frame);
    }
    return names;
}

/** Writes the archive's animation trees (hierarchy and object names; keyframes are pending). */
void unpackAnimations(const AssetLocator& locator, const std::filesystem::path& outDir,
                      Summary& summary) {
    const auto animPath = locator.find("anim.ps2");
    if (!animPath.has_value()) {
        return;
    }
    try {
        const AnimationFile file = AnimationFile::parse(readFile(*animPath));
        JsonWriter json;
        json.beginObject();
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
                json.key("flags").value(u32{sequence.flags});
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
            writePng(outDir / file, decodeGcTexture(bitmap, texturesFile));
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
                decodeGeometryStream(sub.geometry, sub.textureIndex, mesh);
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
void unpackAudio(const std::filesystem::path& directory, const std::filesystem::path& outDir,
                 Summary& summary) {
    const AssetLocator locator(directory);
    const auto romPath = locator.find("audatps2.rom");
    if (!romPath.has_value()) {
        print("audio: AUDATPS2.ROM not found");
        return;
    }
    const AudioRom rom = AudioRom::parse(readFile(*romPath));
    for (const AudioRomBank& bank : rom.banks) {
        const auto bankPath = locator.find(bank.file + ".vbk");
        if (!bankPath.has_value()) {
            print(std::format("audio: bank {} has no .vbk file", bank.name));
            continue;
        }
        print(std::format("audio bank {}", bank.name));
        try {
            const SoundBank sounds = SoundBank::parse(readFile(*bankPath));
            const std::filesystem::path bankDir = outDir / bank.name;
            std::filesystem::create_directories(bankDir / "samples");

            JsonWriter json;
            json.beginObject();
            json.key("bank").value(bank.name);
            json.key("sounds").beginArray();
            for (u32 i = 0; i < bank.soundCount; ++i) {
                const AudioRomSound& entry = rom.sounds[bank.firstSound + i];
                json.beginObject();
                json.key("index").value(i);
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
                const std::vector<s16> pcm = decodeBankSample(sample);
                writeFile(bankDir / file, encodeWav(pcm, sample.sampleRate, 1));
                json.beginObject();
                json.key("index").value(static_cast<u64>(i));
                json.key("name").value(sample.name);
                json.key("file").value(file);
                json.key("sampleRate").value(sample.sampleRate);
                json.key("frames").value(static_cast<u64>(pcm.size()));
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
        } catch (const std::exception& e) {
            ++summary.failures;
            print(std::format("  bank {}: {}", bank.name, e.what()));
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

/** Writes a level's placed objects and marker points. */
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
        json.key("animations").value(world.animationCount);
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
            json.endObject();
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
        ++summary.worlds;
    } catch (const std::exception& e) {
        ++summary.failures;
        print(std::format("  world: {}", e.what()));
    }
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
    json.key("effects").value(record.effectCount);
    json.key("damage").value(record.damageCount);
    json.endObject();
    std::filesystem::create_directories(outDir);
    writeTextFile(outDir / (normalizeAssetName(file.stem().string()) + ".json"), json.take());
    ++summary.classes;
}

int run(const std::filesystem::path& assetRoot, const std::filesystem::path& outRoot,
        std::string_view only, bool levels) {
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
        if (!only.empty() && upper != normalizeAssetName(only) && upper != "LEVELS") {
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
    print(std::format("{} archives, {} textures, {} models, {} animation trees, {} fonts, "
                      "{} text roms, {} sound banks, {} samples, {} classes, {} worlds, "
                      "{} failures",
                      summary.archives, summary.textures, summary.models, summary.animations,
                      summary.fonts, summary.textRoms, summary.banks, summary.samples,
                      summary.classes, summary.worlds, summary.failures));
    return summary.failures == 0 ? 0 : 3;
}

} // namespace

int main(int argc, char* argv[]) {
    std::vector<std::string_view> args;
    const std::span<char*> rawArgs(argv, static_cast<usize>(argc));
    for (const char* arg : rawArgs.subspan(rawArgs.empty() ? 0 : 1)) {
        args.emplace_back(arg);
    }
    std::string_view only;
    bool levels = false;
    std::vector<std::string_view> positional;
    for (usize i = 0; i < args.size(); ++i) {
        if (args[i] == "--only" && i + 1 < args.size()) {
            only = args[++i];
        } else if (args[i] == "--levels") {
            levels = true;
        } else {
            positional.push_back(args[i]);
        }
    }
    if (positional.size() != 2) {
        print("usage: gdlunpack <asset-root> <output-root> [--only <directory|level>] [--levels]");
        return 2;
    }
    try {
        return run(positional[0], positional[1], only, levels);
    } catch (const std::exception& e) {
        print(std::format("error: {}", e.what()));
        return 1;
    }
}
