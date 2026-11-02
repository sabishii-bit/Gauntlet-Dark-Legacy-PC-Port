#include "formats/ParticleTemplate.h"

#include "engine/core/Error.h"
#include "engine/core/Types.h"
#include "engine/io/ByteReader.h"

namespace gdl::formats {

namespace {

constexpr usize kNameSize = 32;

Vec3 readVec3(ByteReader& reader) {
    const f32 x = reader.readF32();
    const f32 y = reader.readF32();
    const f32 z = reader.readF32();
    return Vec3{x, y, z};
}

} // namespace

ParticleTemplateRecord readParticleTemplate(std::span<const u8> record) {
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
    out.maxParticles = static_cast<s32>(reader.readU32());
    out.maxDirections = reader.readU32();
    out.maxPositions = reader.readU32();
    for (f32& value : out.emitterLife) {
        value = reader.readF32();
    }
    for (f32& value : out.particleLife) {
        value = reader.readF32();
    }
    reader.readU32();
    reader.readU32();
    out.angle = reader.readF32();
    out.textureCount = static_cast<s32>(reader.readU32());
    for (usize i = 0; i < kNameSize; ++i) {
        const auto c = static_cast<char>(reader.readU8());
        if (c != 0 && out.texture.size() == i) {
            out.texture.push_back(c);
        }
    }
    out.direction = readVec3(reader);
    out.volume = readVec3(reader);
    for (f32& value : out.rate) {
        value = reader.readF32();
    }
    out.rateRandom = reader.readF32();
    out.gravity = reader.readF32();
    out.drag = reader.readF32();
    out.speed = reader.readF32();
    for (u32& value : out.rgba) {
        value = reader.readU32();
    }
    for (f32& value : out.width) {
        value = reader.readF32();
    }
    out.delay = reader.readF32();
    return out;
}

} // namespace gdl::formats
