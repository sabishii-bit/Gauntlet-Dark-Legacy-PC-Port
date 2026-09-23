#include "formats/ParticleTemplate.h"

#include <bit>
#include <cstddef>
#include <cstdint>

#include "engine/core/Error.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {

namespace {

constexpr std::size_t kNameSize = 32;

float readF32(ByteReader& reader) {
    return std::bit_cast<float>(reader.readU32());
}

Vec3 readVec3(ByteReader& reader) {
    const float x = readF32(reader);
    const float y = readF32(reader);
    const float z = readF32(reader);
    return Vec3{x, y, z};
}

} // namespace

ParticleTemplateRecord readParticleTemplate(std::span<const std::uint8_t> record) {
    if (record.size() < ParticleTemplateRecord::kSize) {
        throw FormatError("particle template record is too short");
    }
    ByteReader reader(record);
    ParticleTemplateRecord out;
    reader.readU32(); // version
    out.preset = reader.readU16();
    out.id = static_cast<char>(reader.readU8());
    reader.readU8();
    out.flags = reader.readU32();
    out.flagMask = reader.readU32();
    out.enables = reader.readU32();
    out.maxParticles = static_cast<std::int32_t>(reader.readU32());
    out.maxDirections = reader.readU32();
    out.maxPositions = reader.readU32();
    for (float& value : out.emitterLife) {
        value = readF32(reader);
    }
    for (float& value : out.particleLife) {
        value = readF32(reader);
    }
    reader.readU32();
    reader.readU32();
    out.angle = readF32(reader);
    out.textureCount = static_cast<std::int32_t>(reader.readU32());
    for (std::size_t i = 0; i < kNameSize; ++i) {
        const auto c = static_cast<char>(reader.readU8());
        if (c != 0 && out.texture.size() == i) {
            out.texture.push_back(c);
        }
    }
    out.direction = readVec3(reader);
    out.volume = readVec3(reader);
    for (float& value : out.rate) {
        value = readF32(reader);
    }
    out.rateRandom = readF32(reader);
    out.gravity = readF32(reader);
    out.drag = readF32(reader);
    out.speed = readF32(reader);
    for (std::uint32_t& value : out.rgba) {
        value = reader.readU32();
    }
    for (float& value : out.width) {
        value = readF32(reader);
    }
    out.delay = readF32(reader);
    return out;
}

} // namespace gdl::formats
