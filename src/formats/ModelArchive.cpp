#include "formats/ModelArchive.h"

#include <algorithm>
#include <bit>

#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/core/Types.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {

namespace {

constexpr usize kVersionHeaderSize = 68;
constexpr usize kHeaderSize = 92;
constexpr usize kObjectRecordSize = 64;
constexpr usize kBitmapRecordSize = 64;
constexpr usize kObjectDefSize = 24;
constexpr usize kBitmapDefSize = 36;
constexpr usize kSubObjectSize = 8;
constexpr usize kQuadword = 16;
constexpr usize kNameLength = 16;
constexpr usize kBitmapNameLength = 30;

std::string readName(std::span<const u8> bytes, usize offset, usize length) {
    std::string name;
    for (usize i = 0; i < length; ++i) {
        const u8 c = bytes[offset + i];
        if (c == 0) {
            break;
        }
        name.push_back(static_cast<char>(c));
    }
    return name;
}

f32 readF32LE(std::span<const u8> bytes, usize offset) {
    return std::bit_cast<f32>(readU32LE(bytes, offset));
}

void require(std::span<const u8> file, usize offset, usize size, const char* what) {
    if (offset > file.size() || size > file.size() - offset) {
        throw FormatError(std::string("objects archive: ") + what + " lies outside the file");
    }
}

} // namespace

ModelArchive ModelArchive::parse(std::span<const u8> file) {
    if (file.size() < kVersionHeaderSize + kHeaderSize) {
        throw FormatError("objects archive is too small for its header");
    }
    ModelArchive archive;
    archive.m_sourceDirectory = readName(file, 0, 32);
    archive.m_version = readU32LE(file, 64);
    if (archive.m_version != kVersion13 && archive.m_version != kVersion12) {
        throw FormatError("objects archive has an unsupported version");
    }

    ByteReader header(file.subspan(kVersionHeaderSize, kHeaderSize));
    const u32 objectCount = header.readU32();
    const u32 bitmapCount = header.readU32();
    const u32 objectDefCount = header.readU32();
    const u32 bitmapDefCount = header.readU32();
    const u32 objectsOffset = header.readU32();
    const u32 bitmapsOffset = header.readU32();
    const u32 objectDefsOffset = header.readU32();
    const u32 bitmapDefsOffset = header.readU32();

    require(file, objectsOffset, usize{objectCount} * kObjectRecordSize, "object table");
    require(file, bitmapsOffset, usize{bitmapCount} * kBitmapRecordSize, "bitmap table");
    require(file, objectDefsOffset, usize{objectDefCount} * kObjectDefSize, "object names");
    require(file, bitmapDefsOffset, usize{bitmapDefCount} * kBitmapDefSize, "bitmap names");

    for (u32 i = 0; i < objectDefCount; ++i) {
        const usize at = objectDefsOffset + usize{i} * kObjectDefSize;
        ArchiveObjectDef def;
        def.name = readName(file, at, kNameLength);
        def.boundingRadius = readF32LE(file, at + 16);
        def.objectIndex = static_cast<s16>(readU16LE(file, at + 20));
        def.frames = static_cast<s16>(readU16LE(file, at + 22));
        archive.m_objectDefs.push_back(std::move(def));
    }

    for (u32 i = 0; i < bitmapDefCount; ++i) {
        const usize at = bitmapDefsOffset + usize{i} * kBitmapDefSize;
        ArchiveBitmapDef def;
        def.name = readName(file, at, kBitmapNameLength);
        def.textureIndex = readU16LE(file, at + 30);
        def.width = readU16LE(file, at + 32);
        def.height = readU16LE(file, at + 34);
        if (!def.name.empty()) {
            archive.m_bitmapDefs.push_back(std::move(def));
        }
    }

    for (u32 i = 0; i < bitmapCount; ++i) {
        const usize at = bitmapsOffset + usize{i} * kBitmapRecordSize;
        ArchiveBitmap bitmap;
        bitmap.format = file[at];
        bitmap.lodK = static_cast<s8>(file[at + 1]);
        bitmap.mipmapCount = file[at + 2];
        bitmap.flags = readU16LE(file, at + 8);
        bitmap.dataOffset = readU32LE(file, at + 12);
        bitmap.frameCount = readU16LE(file, at + 20);
        bitmap.width = readU16LE(file, at + 22);
        bitmap.height = readU16LE(file, at + 24);
        archive.m_bitmaps.push_back(bitmap);
    }

    for (u32 i = 0; i < objectCount; ++i) {
        const usize at = objectsOffset + usize{i} * kObjectRecordSize;
        ArchiveObject object;
        object.inverseRadius = readF32LE(file, at);
        object.boundingRadius = readF32LE(file, at + 4);
        object.flags = readU32LE(file, at + 8);
        const auto subObjectCount = static_cast<s32>(readU32LE(file, at + 12));
        const u32 subObjectsOffset = readU32LE(file, at + 24);
        const u32 modelsOffset = readU32LE(file, at + 28);
        object.vertexCount = static_cast<s32>(readU32LE(file, at + 32));
        object.triangleCount = static_cast<s32>(readU32LE(file, at + 36));
        object.id = static_cast<s32>(readU32LE(file, at + 40));

        for (s32 s = 0; s < subObjectCount; ++s) {
            ArchiveSubObject sub;
            const usize recordAt =
                s == 0 ? at + 16
                       : subObjectsOffset + usize{static_cast<u32>(s - 1)} * kSubObjectSize;
            require(file, recordAt, kSubObjectSize, "sub-object record");
            sub.quadwordCount = readU16LE(file, recordAt);
            sub.textureIndex = readU16LE(file, recordAt + 2);
            sub.lightmapIndex = readU16LE(file, recordAt + 4);
            sub.lodK = static_cast<s16>(readU16LE(file, recordAt + 6));
            object.subObjects.push_back(std::move(sub));
        }

        usize modelAt = modelsOffset;
        for (ArchiveSubObject& sub : object.subObjects) {
            if (modelAt == 0 || sub.quadwordCount == 0) {
                break;
            }
            require(file, modelAt, kQuadword, "geometry header");
            const u16 quadwords = readU16LE(file, modelAt);
            require(file, modelAt + kQuadword, usize{quadwords} * kQuadword, "geometry stream");
            const auto stream = file.subspan(modelAt, (usize{quadwords} + 1) * kQuadword);
            sub.geometry.assign(stream.begin(), stream.end());
            modelAt += (usize{quadwords} + 1) * kQuadword;
        }
        archive.m_objects.push_back(std::move(object));
    }
    return archive;
}

std::optional<u32> ModelArchive::findBitmap(std::string_view name) const {
    const std::string key = normalizeAssetName(name);
    const auto it = std::ranges::find_if(m_bitmapDefs, [&key](const ArchiveBitmapDef& def) {
        return normalizeAssetName(def.name) == key;
    });
    if (it == m_bitmapDefs.end()) {
        return std::nullopt;
    }
    return it->textureIndex;
}

std::optional<u32> ModelArchive::findObject(std::string_view name) const {
    const std::string key = normalizeAssetName(name);
    const auto it = std::ranges::find_if(m_objectDefs, [&key](const ArchiveObjectDef& def) {
        return normalizeAssetName(def.name) == key;
    });
    if (it == m_objectDefs.end()) {
        return std::nullopt;
    }
    return static_cast<u32>(it->objectIndex);
}

} // namespace gdl::formats
