#include "formats/ModelArchive.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>

#include "engine/core/Error.h"
#include "engine/core/Strings.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {

namespace {

constexpr std::size_t kVersionHeaderSize = 68;
constexpr std::size_t kHeaderSize = 92;
constexpr std::size_t kObjectRecordSize = 64;
constexpr std::size_t kBitmapRecordSize = 64;
constexpr std::size_t kObjectDefSize = 24;
constexpr std::size_t kBitmapDefSize = 36;
constexpr std::size_t kSubObjectSize = 8;
constexpr std::size_t kQuadword = 16;
constexpr std::size_t kNameLength = 16;
constexpr std::size_t kBitmapNameLength = 30;

std::string readName(std::span<const std::uint8_t> bytes, std::size_t offset, std::size_t length) {
    std::string name;
    for (std::size_t i = 0; i < length; ++i) {
        const std::uint8_t c = bytes[offset + i];
        if (c == 0) {
            break;
        }
        name.push_back(static_cast<char>(c));
    }
    return name;
}

float readF32LE(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return std::bit_cast<float>(readU32LE(bytes, offset));
}

void require(std::span<const std::uint8_t> file, std::size_t offset, std::size_t size,
             const char* what) {
    if (offset > file.size() || size > file.size() - offset) {
        throw FormatError(std::string("objects archive: ") + what + " lies outside the file");
    }
}

} // namespace

ModelArchive ModelArchive::parse(std::span<const std::uint8_t> file) {
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
    const std::uint32_t objectCount = header.readU32();
    const std::uint32_t bitmapCount = header.readU32();
    const std::uint32_t objectDefCount = header.readU32();
    const std::uint32_t bitmapDefCount = header.readU32();
    const std::uint32_t objectsOffset = header.readU32();
    const std::uint32_t bitmapsOffset = header.readU32();
    const std::uint32_t objectDefsOffset = header.readU32();
    const std::uint32_t bitmapDefsOffset = header.readU32();

    require(file, objectsOffset, std::size_t{objectCount} * kObjectRecordSize, "object table");
    require(file, bitmapsOffset, std::size_t{bitmapCount} * kBitmapRecordSize, "bitmap table");
    require(file, objectDefsOffset, std::size_t{objectDefCount} * kObjectDefSize, "object names");
    require(file, bitmapDefsOffset, std::size_t{bitmapDefCount} * kBitmapDefSize, "bitmap names");

    for (std::uint32_t i = 0; i < objectDefCount; ++i) {
        const std::size_t at = objectDefsOffset + std::size_t{i} * kObjectDefSize;
        ArchiveObjectDef def;
        def.name = readName(file, at, kNameLength);
        def.boundingRadius = readF32LE(file, at + 16);
        def.objectIndex = static_cast<std::int16_t>(readU16LE(file, at + 20));
        def.frames = static_cast<std::int16_t>(readU16LE(file, at + 22));
        archive.m_objectDefs.push_back(std::move(def));
    }

    for (std::uint32_t i = 0; i < bitmapDefCount; ++i) {
        const std::size_t at = bitmapDefsOffset + std::size_t{i} * kBitmapDefSize;
        ArchiveBitmapDef def;
        def.name = readName(file, at, kBitmapNameLength);
        def.textureIndex = readU16LE(file, at + 30);
        def.width = readU16LE(file, at + 32);
        def.height = readU16LE(file, at + 34);
        if (!def.name.empty()) {
            archive.m_bitmapDefs.push_back(std::move(def));
        }
    }

    for (std::uint32_t i = 0; i < bitmapCount; ++i) {
        const std::size_t at = bitmapsOffset + std::size_t{i} * kBitmapRecordSize;
        ArchiveBitmap bitmap;
        bitmap.format = file[at];
        bitmap.lodK = static_cast<std::int8_t>(file[at + 1]);
        bitmap.mipmapCount = file[at + 2];
        bitmap.flags = readU16LE(file, at + 8);
        bitmap.dataOffset = readU32LE(file, at + 12);
        bitmap.frameCount = readU16LE(file, at + 20);
        bitmap.width = readU16LE(file, at + 22);
        bitmap.height = readU16LE(file, at + 24);
        archive.m_bitmaps.push_back(bitmap);
    }

    for (std::uint32_t i = 0; i < objectCount; ++i) {
        const std::size_t at = objectsOffset + std::size_t{i} * kObjectRecordSize;
        ArchiveObject object;
        object.inverseRadius = readF32LE(file, at);
        object.boundingRadius = readF32LE(file, at + 4);
        object.flags = readU32LE(file, at + 8);
        const auto subObjectCount = static_cast<std::int32_t>(readU32LE(file, at + 12));
        const std::uint32_t subObjectsOffset = readU32LE(file, at + 24);
        const std::uint32_t modelsOffset = readU32LE(file, at + 28);
        object.vertexCount = static_cast<std::int32_t>(readU32LE(file, at + 32));
        object.triangleCount = static_cast<std::int32_t>(readU32LE(file, at + 36));
        object.id = static_cast<std::int32_t>(readU32LE(file, at + 40));

        for (std::int32_t s = 0; s < subObjectCount; ++s) {
            ArchiveSubObject sub;
            const std::size_t recordAt =
                s == 0 ? at + 16
                       : subObjectsOffset +
                             std::size_t{static_cast<std::uint32_t>(s - 1)} * kSubObjectSize;
            require(file, recordAt, kSubObjectSize, "sub-object record");
            sub.quadwordCount = readU16LE(file, recordAt);
            sub.textureIndex = readU16LE(file, recordAt + 2);
            sub.lightmapIndex = readU16LE(file, recordAt + 4);
            sub.lodK = static_cast<std::int16_t>(readU16LE(file, recordAt + 6));
            object.subObjects.push_back(std::move(sub));
        }

        std::size_t modelAt = modelsOffset;
        for (ArchiveSubObject& sub : object.subObjects) {
            if (modelAt == 0 || sub.quadwordCount == 0) {
                break;
            }
            require(file, modelAt, kQuadword, "geometry header");
            const std::uint16_t quadwords = readU16LE(file, modelAt);
            require(file, modelAt + kQuadword, std::size_t{quadwords} * kQuadword,
                    "geometry stream");
            const auto stream = file.subspan(modelAt, (std::size_t{quadwords} + 1) * kQuadword);
            sub.geometry.assign(stream.begin(), stream.end());
            modelAt += (std::size_t{quadwords} + 1) * kQuadword;
        }
        archive.m_objects.push_back(std::move(object));
    }
    return archive;
}

std::optional<std::uint32_t> ModelArchive::findBitmap(std::string_view name) const {
    const std::string key = normalizeAssetName(name);
    const auto it = std::ranges::find_if(m_bitmapDefs, [&key](const ArchiveBitmapDef& def) {
        return normalizeAssetName(def.name) == key;
    });
    if (it == m_bitmapDefs.end()) {
        return std::nullopt;
    }
    return it->textureIndex;
}

std::optional<std::uint32_t> ModelArchive::findObject(std::string_view name) const {
    const std::string key = normalizeAssetName(name);
    const auto it = std::ranges::find_if(m_objectDefs, [&key](const ArchiveObjectDef& def) {
        return normalizeAssetName(def.name) == key;
    });
    if (it == m_objectDefs.end()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(it->objectIndex);
}

} // namespace gdl::formats
